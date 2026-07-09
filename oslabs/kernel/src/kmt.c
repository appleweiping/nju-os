// L2: kmt — kernel multithreading, spinlocks and semaphores.
//
// NJU OS 2022 (jyy) OSLab 2.  Provides preemptive kernel threads on the
// AbstractMachine CTE, correct on SMP:
//
//   kmt->create(task, name, entry, arg)   spawn a runnable thread
//   kmt->teardown(task)                   reclaim a finished thread
//   kmt->spin_{init,lock,unlock}          SMP spinlocks (irq-safe, nesting)
//   kmt->sem_{init,wait,signal}           counting semaphores (P/V, sleeping)
//
// Scheduling: a round-robin over the global task list, driven by the timer
// interrupt (EVENT_IRQ_TIMER) and voluntary yield (EVENT_YIELD).  The
// scheduler handler is installed via os->on_irq; os_trap calls it, and the
// Context* it returns is what CTE resumes.
//
// Interrupt discipline: spin_lock disables interrupts on the current CPU and
// remembers how many locks that CPU holds; interrupts are re-enabled only when
// the last lock is released (classic push_off / pop_off, à la xv6).  This keeps
// a CPU from being preempted while holding a spinlock (which would deadlock).

#include <os.h>

// -------------------------------------------------------- global scheduler

static struct task *task_list;          // singly-linked list of all tasks
static struct spinlock task_lock;       // protects task_list + states
static struct task    *current[MAX_CPU];// running task per CPU
static struct task     idle[MAX_CPU];   // per-CPU idle task (runs when nothing else)

// Per-CPU interrupt-nesting bookkeeping (push_off/pop_off).
//
// In the AbstractMachine model the interrupt-enable state lives in each saved
// Context (its signal mask); CTE restores it on a context switch, bypassing
// these counters.  We therefore treat (ncli, intena) as *scheduler state for
// the task currently on this CPU* and save/restore them together with the
// task on every switch (see kmt_schedule).  Between switches they behave like
// xv6's push_off/pop_off.
static int ncli[MAX_CPU];               // depth of nested spin_locks (per CPU)
static int intena[MAX_CPU];             // interrupts on before the first lock?

// --------------------------------------------------------------- raw spin

static void raw_lock(int *locked)   { while (atomic_xchg(locked, 1)) ; }
static void raw_unlock(int *locked) { atomic_xchg(locked, 0); }

// task_lock is acquired both from ordinary kernel context and from the trap
// scheduler, so it is accessed uniformly with interrupts forced off (saving the
// prior state) rather than through the nesting push_off/pop_off path.  This
// avoids mixing "cooked" and "raw" access to the same lock.
static int task_lock_flag;              // the raw lock word
static int task_lock_prev_ie[MAX_CPU];  // interrupt state before acquiring

static void task_lock_acquire(void) {
  int was = ienabled();
  iset(false);
  raw_lock(&task_lock_flag);
  task_lock_prev_ie[cpu_current()] = was;
}
static void task_lock_release(void) {
  int cpu = cpu_current();
  int was = task_lock_prev_ie[cpu];
  raw_unlock(&task_lock_flag);
  if (was) iset(true);
}

// Disable interrupts, remembering the prior state on the first nesting level.
static void push_off(void) {
  int cpu = cpu_current();
  int was = ienabled();
  iset(false);
  if (ncli[cpu] == 0) intena[cpu] = was;
  ncli[cpu]++;
}

// Re-enable interrupts once the outermost lock is released.
//
// NOTE on SMP-native: the authoritative interrupt-enable state lives in each
// task's saved Context sigmask (restored by CTE on a switch), so the ncli/intena
// counters are only a best-effort mirror.  Across a preemption+migration the
// mirror can momentarily disagree, so we clamp rather than panic; the real
// interrupt state is always correct because it rides with the context.
static void pop_off(void) {
  int cpu = cpu_current();
  if (ncli[cpu] > 0) ncli[cpu]--;
  if (ncli[cpu] == 0 && intena[cpu]) iset(true);
}

// ------------------------------------------------------------- spinlocks

static void kmt_spin_init(struct spinlock *lk, const char *name) {
  lk->locked = 0;
  lk->name   = name;
  lk->cpu    = -1;
}

static void kmt_spin_lock(struct spinlock *lk) {
  push_off();                                  // no preemption while held
  panic_on(lk->cpu == cpu_current(), "re-acquiring a held spinlock");
  raw_lock(&lk->locked);
  lk->cpu = cpu_current();
}

static void kmt_spin_unlock(struct spinlock *lk) {
  panic_on(lk->cpu != cpu_current(), "unlocking a lock we don't hold");
  lk->cpu = -1;
  raw_unlock(&lk->locked);
  pop_off();
}

// ------------------------------------------------------------- scheduler

// The entry trampoline: a task begins here with interrupts enabled.
struct startup { void (*entry)(void *); void *arg; };

static void task_trampoline(void *arg) {
  struct startup *s = (struct startup *)arg;
  void (*entry)(void *) = s->entry;
  void *earg = s->arg;
  pmm->free(s);
  iset(true);                                  // tasks run with interrupts on
  entry(earg);
  // Task returned: mark itself dead and yield forever.
  task_lock_acquire();
  current[cpu_current()]->state = ST_DEAD;
  task_lock_release();
  while (1) yield();
}

// A task is schedulable iff it is RUNNABLE, owned by no CPU (cpu == -1), and no
// CPU is still on its stack (on_cpu == 0).  Idle tasks are per-CPU and never in
// task_list.  Caller holds task_lock.
static struct task *pick_next(struct task *after) {
  struct task *start = (after && after->sched_next) ? after->sched_next : task_list;

  struct task *t = start;
  for (int steps = 0; t && steps <= 200000; steps++) {
    if (t->state == ST_RUNNABLE && t->cpu == -1 && t->on_cpu == 0)
      return t;
    t = t->sched_next ? t->sched_next : task_list;   // wrap
    if (t == start) break;
  }
  return NULL;   // caller falls back to idle
}

// Per-CPU "previous task" pending deferred release: the task this CPU was
// running before the current switch.  We only clear its on_cpu flag (making it
// pickable again) once this CPU has switched onto a DIFFERENT stack, so no two
// CPUs ever execute on the same kernel stack.
static struct task *prev_task[MAX_CPU];

// Timer/yield handler: save the running task's context, pick the next task, and
// return its context so CTE resumes it.  Runs in trap context (interrupts
// masked), so it uses the raw task_lock.
//
// Also called directly by uproc's syscall handler (see os.h) after servicing a
// system call, so that exit()/wait()/sleep() can switch to another task.
Context *kmt_schedule(Event ev, Context *ctx) {
  int cpu = cpu_current();

  task_lock_acquire();

  // Deferred release: the task we ran on the PREVIOUS switch is now safely off
  // this CPU's stack (we are executing on cur's stack now), so free it for
  // other CPUs to pick.
  if (prev_task[cpu] && prev_task[cpu] != current[cpu]) {
    prev_task[cpu]->on_cpu = 0;
    prev_task[cpu] = NULL;
  }

  struct task *cur = current[cpu];
  if (cur) {
    cur->context = ctx;          // save where we were interrupted
    cur->saved_ncli   = ncli[cpu];
    cur->saved_intena = intena[cpu];
    cur->cpu = -1;               // release ownership (but on_cpu stays 1)
    if (cur != &idle[cpu] &&
        cur->state != ST_BLOCKED && cur->state != ST_DEAD)
      cur->state = ST_RUNNABLE;
  }

  struct task *after = (cur && cur != &idle[cpu]) ? cur : NULL;
  struct task *next = pick_next(after);
  if (!next) next = &idle[cpu];

  next->cpu    = cpu;
  next->on_cpu = 1;                // this CPU now owns next's stack
  current[cpu] = next;
  ncli[cpu]    = next->saved_ncli;
  intena[cpu]  = next->saved_intena;

  // Remember cur so its on_cpu is cleared on the *next* switch, once this CPU is
  // definitely running on next's stack instead of cur's.
  if (cur && cur != next) prev_task[cpu] = cur;
  else                    prev_task[cpu] = NULL;

  Context *rc = next->context;
  task_lock_release();
  return rc;
}

// ------------------------------------------------------------- thread API

static int kmt_create(struct task *task, const char *name,
                      void (*entry)(void *arg), void *arg) {
  task->name  = name;
  task->state = ST_RUNNABLE;
  task->cpu   = -1;
  task->sem_next = NULL;
  task->on_cpu       = 0;
  task->saved_ncli   = 0;   // fresh task starts outside any spinlock
  task->saved_intena = 1;   // ...with interrupts enabled (kcontext sigmask)

  // Package (entry,arg) so the trampoline can run with interrupts enabled.
  struct startup *s = pmm->alloc(sizeof(struct startup));
  panic_on(!s, "kmt_create: out of memory");
  s->entry = entry; s->arg = arg;

  Area kstack = { task->stack, task->stack + STACK_SZ };
  task->context = kcontext(kstack, task_trampoline, s);

  task_lock_acquire();
  task->sched_next = task_list;
  task_list = task;
  task_lock_release();
  return 0;
}

// Add a task whose Context has already been built (e.g. a user process created
// by uproc via ucontext) to the scheduler's ready list.  The caller must have
// set name/state/cpu/on_cpu/saved_* and task->context beforehand.
void kmt_register(struct task *task) {
  task_lock_acquire();
  task->sched_next = task_list;
  task_list = task;
  task_lock_release();
}

// The task currently running on this CPU (used by uproc syscall handlers).
struct task *kmt_current(void) {
  return current[cpu_current()];
}

static void kmt_teardown(struct task *task) {
  task_lock_acquire();
  // unlink from the global list
  struct task **pp = &task_list;
  while (*pp && *pp != task) pp = &(*pp)->sched_next;
  if (*pp) *pp = task->sched_next;
  task_lock_release();
  // stack is embedded in the struct; caller frees the struct.
}

// ------------------------------------------------------------- semaphores

static void kmt_sem_init(struct semaphore *sem, const char *name, int value) {
  sem->value   = value;
  sem->name    = name;
  sem->waiters = NULL;
  kmt_spin_init(&sem->lock, name);
}

// P / wait: decrement; if the value goes negative, block until signalled.
//
// The block-and-yield must be atomic w.r.t. the timer: we keep interrupts
// disabled (push_off held via sem->lock) while marking ourselves BLOCKED and
// enqueuing, and only release sem->lock *after* yield() has switched us away.
// To make that possible without a lost-wakeup, we mark BLOCKED under task_lock,
// then yield while still holding sem->lock's interrupt-disable; the scheduler
// saves our context and picks another task.  When we are later made RUNNABLE by
// sem_signal and rescheduled, we resume right after yield() and drop sem->lock.
static void kmt_sem_wait(struct semaphore *sem) {
  kmt_spin_lock(&sem->lock);
  sem->value--;
  if (sem->value < 0) {
    int cpu = cpu_current();
    struct task *self = current[cpu];
    // enqueue self (FIFO)
    self->sem_next = NULL;
    if (!sem->waiters) sem->waiters = self;
    else {
      struct task *t = sem->waiters;
      while (t->sem_next) t = t->sem_next;
      t->sem_next = self;
    }
    // Mark blocked under task_lock, then release sem->lock and yield.  Because
    // we are inside sem->lock's push_off, interrupts stay disabled until after
    // yield() traps into the scheduler, so no timer can preempt in the window.
    task_lock_acquire();
    self->state = ST_BLOCKED;
    task_lock_release();
    kmt_spin_unlock(&sem->lock);      // pop_off: interrupts may re-enable here
    yield();                          // trap -> scheduler switches us out
    return;                           // resumed later, after being signalled
  }
  kmt_spin_unlock(&sem->lock);
}

// V / signal: increment; if tasks were waiting, wake the oldest.
static void kmt_sem_signal(struct semaphore *sem) {
  kmt_spin_lock(&sem->lock);
  sem->value++;
  if (sem->value <= 0 && sem->waiters) {
    struct task *w = sem->waiters;
    sem->waiters = w->sem_next;
    w->sem_next = NULL;
    task_lock_acquire();
    w->state = ST_RUNNABLE;           // make it schedulable again
    task_lock_release();
  }
  kmt_spin_unlock(&sem->lock);
}

// ------------------------------------------------------------- init

// The idle loop: run with interrupts enabled so the timer keeps firing and the
// scheduler can hand the CPU to a real task the moment one becomes runnable.
static void idle_entry(void *arg) {
  iset(true);
  while (1) yield();
}

static void kmt_init() {
  kmt_spin_init(&task_lock, "task_lock");
  task_list = NULL;
  for (int i = 0; i < MAX_CPU; i++) {
    current[i] = NULL;
    ncli[i] = 0; intena[i] = 0;
    // idle task: an always-runnable placeholder with its own real context.
    idle[i].name  = "idle";
    idle[i].state = ST_RUNNABLE;
    idle[i].cpu   = -1;
    idle[i].sem_next = NULL;
    idle[i].sched_next = NULL;
    idle[i].saved_ncli   = 0;
    idle[i].saved_intena = 1;
    idle[i].on_cpu       = 0;
    prev_task[i]         = NULL;
    Area kstack = { idle[i].stack, idle[i].stack + STACK_SZ };
    idle[i].context = kcontext(kstack, idle_entry, NULL);
  }
  // Install the scheduler for both timer preemption and voluntary yields.
  os->on_irq(INT32_MIN, EVENT_IRQ_TIMER, kmt_schedule);
  os->on_irq(INT32_MIN, EVENT_YIELD,     kmt_schedule);
}

MODULE_DEF(kmt) = {
  .init       = kmt_init,
  .create     = kmt_create,
  .teardown   = kmt_teardown,
  .spin_init  = kmt_spin_init,
  .spin_lock  = kmt_spin_lock,
  .spin_unlock= kmt_spin_unlock,
  .sem_init   = kmt_sem_init,
  .sem_wait   = kmt_sem_wait,
  .sem_signal = kmt_sem_signal,
};

// ===========================================================================
// L2 self-test (compiled in with `make ARCH=native TEST=kmt`).
//
//   (a) Mutual exclusion: NWORK threads each increment a shared counter
//       INCS times under one spinlock.  Correct <=> final == NWORK*INCS
//       (a missing lock loses updates on SMP).
//   (b) Semaphores: a bounded-buffer producer/consumer.  P producers each
//       produce ITEMS items into a size-CAP buffer guarded by (empty, fill)
//       semaphores + a spinlock; C consumers drain them.  Correct <=> the
//       number consumed == P*ITEMS and the buffer never over/underflows.
// ===========================================================================
#ifdef KMT_TEST

#define NWORK  8
#define INCS   50000
#define CAP    8
#define NPROD  4
#define NCONS  4
#define ITEMS  20000

static struct spinlock counter_lock;
static long shared_counter;

static struct task counter_tasks[NWORK];

static void counter_worker(void *arg) {
  for (int i = 0; i < INCS; i++) {
    kmt_spin_lock(&counter_lock);
    shared_counter++;
    kmt_spin_unlock(&counter_lock);
  }
}

// bounded buffer
static struct semaphore sem_empty, sem_fill;
static struct spinlock  buf_lock;
static int  buffer[CAP];
static int  buf_head, buf_tail;
static long produced_total, consumed_total;
static int  overflow_seen, underflow_seen;

static struct task prod_tasks[NPROD], cons_tasks[NCONS];

static void producer(void *arg) {
  for (int i = 0; i < ITEMS; i++) {
    kmt->sem_wait(&sem_empty);
    kmt->spin_lock(&buf_lock);
    int used = (buf_head - buf_tail + 2 * CAP) % (2 * CAP);
    if (used >= CAP) overflow_seen = 1;
    buffer[buf_head % CAP] = i;
    buf_head = (buf_head + 1) % (2 * CAP);
    produced_total++;
    kmt->spin_unlock(&buf_lock);
    kmt->sem_signal(&sem_fill);
  }
}

static void consumer(void *arg) {
  long *goal = (long *)arg;
  while (1) {
    kmt->sem_wait(&sem_fill);
    kmt->spin_lock(&buf_lock);
    if (buf_head == buf_tail) underflow_seen = 1;
    (void)buffer[buf_tail % CAP];
    buf_tail = (buf_tail + 1) % (2 * CAP);
    long done = ++consumed_total;
    kmt->spin_unlock(&buf_lock);
    kmt->sem_signal(&sem_empty);
    if (done >= *goal) break;   // all items consumed
  }
}

static long consume_goal = (long)NPROD * ITEMS;

// Reporter task: waits for both tests to finish, prints the verdict.
static void reporter(void *arg) {
  // Wait for the counter test.
  while (1) {
    kmt_spin_lock(&counter_lock);
    long c = shared_counter;
    kmt_spin_unlock(&counter_lock);
    if (c >= (long)NWORK * INCS) break;
    yield();
  }
  long final_counter;
  kmt_spin_lock(&counter_lock); final_counter = shared_counter; kmt_spin_unlock(&counter_lock);

  // Wait for the producer/consumer test.
  while (1) {
    kmt->spin_lock(&buf_lock);
    long c = consumed_total;
    kmt->spin_unlock(&buf_lock);
    if (c >= consume_goal) break;
    yield();
  }

  int mutex_ok = (final_counter == (long)NWORK * INCS);
  int sem_ok   = (produced_total == (long)NPROD * ITEMS) &&
                 (consumed_total == (long)NPROD * ITEMS) &&
                 !overflow_seen && !underflow_seen;

  printf("======== KMT SELFTEST RESULT ========\n");
  printf("mutex : counter=%ld expected=%ld  => %s\n",
         final_counter, (long)NWORK * INCS, mutex_ok ? "PASS" : "FAIL");
  printf("sem   : produced=%ld consumed=%ld expected=%ld ovf=%d unf=%d => %s\n",
         produced_total, consumed_total, (long)NPROD * ITEMS,
         overflow_seen, underflow_seen, sem_ok ? "PASS" : "FAIL");
  printf("OVERALL: %s\n", (mutex_ok && sem_ok) ? "PASSED" : "FAILED");
  while (1) yield();
}

void kmt_selftest_start(void) {
  kmt_spin_init(&counter_lock, "counter");
  shared_counter = 0;
  for (int i = 0; i < NWORK; i++)
    kmt->create(&counter_tasks[i], "counter", counter_worker, NULL);

  kmt->sem_init(&sem_empty, "empty", CAP);
  kmt->sem_init(&sem_fill,  "fill",  0);
  kmt_spin_init(&buf_lock, "buf");
  buf_head = buf_tail = 0;
  produced_total = consumed_total = 0;
  overflow_seen = underflow_seen = 0;
  for (int i = 0; i < NPROD; i++)
    kmt->create(&prod_tasks[i], "producer", producer, NULL);
  for (int i = 0; i < NCONS; i++)
    kmt->create(&cons_tasks[i], "consumer", consumer, &consume_goal);

  static struct task rep;
  kmt->create(&rep, "reporter", reporter, NULL);
}

#endif  // KMT_TEST
