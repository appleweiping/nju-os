// L3: uproc — user processes, virtual memory and the system-call interface.
//
// NJU OS 2022 (jyy) OSLab 3, on the AbstractMachine *native* backend.
//
// A user process is a kmt task that carries its own protected address space
// (AddrSpace, built with protect()/map()) and whose saved Context is a *user*
// context (built with ucontext()).  The kmt scheduler time-slices user
// processes exactly like kernel threads; when a user process makes a system
// call it reads the trap page (0x100000), the AM CTE raises EVENT_SYSCALL, and
// os->trap dispatches here (uproc_syscall).
//
// Address-space layout (native user space is [0x40000000, 0xc0000000)):
//     0x40000000  program image (.text/.rodata/.data) + BSS slack
//     0x50000000  mmap()/heap region, grows upward
//     0xc0000000  top of the user stack (grows down)
//
// Physical pages come from the L1 pmm allocator; the AM VME maps them into the
// user address space.  fork() deep-copies the address space; exit()/wait()
// implement zombie reaping; sleep() blocks against the timer.  Blocking system
// calls (wait/sleep) rewind the trap RIP and re-enter the kernel when woken, so
// no busy-waiting is needed.

#include <os.h>
#include <syscall.h>
#include <user.h>       // PROT_* / MAP_* flags shared with user space

#include "uprogs.inc"   // embedded user-program flat binaries (from user/)

#define PGSIZE       4096
#define USER_BASE    0x40000000UL
#define USER_END     0xc0000000UL      // == USER_SPACE.end on native
#define USER_HEAP    0x50000000UL      // mmap() region base
#define STK_PAGES    32                // 128 KiB user stack
#define BSS_SLACK    (128 * 1024)      // zeroed bytes after the image for .bss

// vfs hooks (weak so L3 links before vfs.c exists; L4 provides the real ones)
int  vfs_available(void) __attribute__((weak));
int  vfs_open (task_t *p, const char *path, int flags) __attribute__((weak));
int  vfs_read (task_t *p, int fd, void *buf, int n)    __attribute__((weak));
int  vfs_write(task_t *p, int fd, const void *buf, int n) __attribute__((weak));
int  vfs_close(task_t *p, int fd)                      __attribute__((weak));
int  vfs_lseek(task_t *p, int fd, int off, int whence) __attribute__((weak));
int  vfs_fstat(task_t *p, int fd, void *st)            __attribute__((weak));
int  vfs_dup  (task_t *p, int fd)                      __attribute__((weak));
int  vfs_mkdir(task_t *p, const char *path)            __attribute__((weak));
int  vfs_link (task_t *p, const char *a, const char *b) __attribute__((weak));
int  vfs_unlink(task_t *p, const char *path)           __attribute__((weak));
int  vfs_chdir(task_t *p, const char *path)            __attribute__((weak));
void vfs_init(void)                                    __attribute__((weak));
void vfs_fork(task_t *parent, task_t *child)           __attribute__((weak));
void vfs_close_all(task_t *p)                          __attribute__((weak));
void vfs_setup_stdio(task_t *p)                        __attribute__((weak));

// --------------------------------------------------------------- global state
static struct spinlock plock;    // protects pid allocation + process list ops
static int next_pid = 1;

// User processes are tracked in a small fixed table (for kill()/reap scans);
// the kmt scheduler owns sched_next, so we do not thread our own list through
// the task struct.
#define MAXPROC 64
static task_t *ptable[MAXPROC];
static int      nptable;

static task_t *initproc;

// ------------------------------------------------------------ page-table glue
static void *up_pgalloc(int size) { return pmm->alloc(size); }
static void  up_pgfree(void *ptr) { pmm->free(ptr); }

// Record + install one user page mapping.
static void up_map(task_t *p, uintptr_t va, void *pa, int prot) {
  panic_on(p->nmap >= PROC_MAXMAP, "too many user mappings");
  p->maps[p->nmap++] = (struct vmap){ .va = va, .pa = pa, .prot = prot };
  map(&p->as, (void *)va, pa, prot);
}

// ------------------------------------------------------------ process table
static void ptable_add(task_t *p) {
  kmt->spin_lock(&plock);
  panic_on(nptable >= MAXPROC, "too many processes");
  ptable[nptable++] = p;
  kmt->spin_unlock(&plock);
}

// ------------------------------------------------------------ proc lifecycle
static task_t *proc_alloc(const char *name) {
  task_t *p = pmm->alloc(sizeof(task_t));
  panic_on(!p, "proc_alloc: out of memory");
  memset(p, 0, sizeof(*p));
  p->name        = name;
  p->is_user     = 1;
  p->state       = ST_RUNNABLE;
  p->cpu         = -1;
  p->on_cpu      = 0;
  p->saved_ncli  = 0;
  p->saved_intena= 1;
  p->heap_top    = USER_HEAP;
  p->maps        = pmm->alloc(sizeof(struct vmap) * PROC_MAXMAP);
  panic_on(!p->maps, "proc_alloc: out of memory (maps)");
  p->nmap        = 0;
  kmt->spin_lock(&plock);
  p->pid = next_pid++;
  kmt->spin_unlock(&plock);
  strcpy(p->cwd, "/");
  protect(&p->as);
  return p;
}

// Free every physical page owned by a process' address space + its bookkeeping.
static void proc_free_mem(task_t *p) {
  for (int i = 0; i < p->nmap; i++) pmm->free(p->maps[i].pa);
  p->nmap = 0;
  pmm->free(p->maps);
  unprotect(&p->as);   // no-op on native, real teardown elsewhere
}

// Load a flat program image at USER_BASE (image + zeroed BSS slack), plus a
// fresh user stack.  Returns the entry point (== USER_BASE).
static uintptr_t proc_load_image(task_t *p, const unsigned char *bin, unsigned len) {
  unsigned need = len + BSS_SLACK;
  unsigned npg  = (need + PGSIZE - 1) / PGSIZE;
  uintptr_t va  = USER_BASE;
  for (unsigned i = 0; i < npg; i++, va += PGSIZE) {
    void *pa = pmm->alloc(PGSIZE);
    panic_on(!pa, "load: out of memory");
    memset(pa, 0, PGSIZE);
    unsigned off = i * PGSIZE;
    if (off < len) {
      unsigned c = len - off; if (c > PGSIZE) c = PGSIZE;
      memcpy(pa, bin + off, c);
    }
    up_map(p, va, pa, MMAP_READ | MMAP_WRITE);
  }
  // user stack: STK_PAGES pages just below USER_END
  for (int i = 0; i < STK_PAGES; i++) {
    void *pa = pmm->alloc(PGSIZE);
    panic_on(!pa, "load: out of stack memory");
    memset(pa, 0, PGSIZE);
    up_map(p, USER_END - (uintptr_t)(i + 1) * PGSIZE, pa, MMAP_READ | MMAP_WRITE);
  }
  return USER_BASE;
}

// Look up an embedded user program by name.
static const uprog_t *uprog_find(const char *name) {
  for (int i = 0; i < nuprogs; i++)
    if (strcmp(uprogs[i].name, name) == 0) return &uprogs[i];
  return NULL;
}

// Build a runnable user process from an embedded program and register it.
static task_t *proc_spawn(const char *name, const unsigned char *bin, unsigned len) {
  task_t *p = proc_alloc(name);
  uintptr_t entry = proc_load_image(p, bin, len);
  Area kstack = { p->stack, p->stack + STACK_SZ };
  p->context = ucontext(&p->as, kstack, (void *)entry);
  if (vfs_setup_stdio) vfs_setup_stdio(p);
  ptable_add(p);
  kmt_register(p);
  return p;
}

// ------------------------------------------------------------ syscall helpers
// Rewind the saved trap RIP back onto the 7-byte syscall instruction, so the
// process re-executes the system call when it is next scheduled (used to block
// wait()/sleep() without busy-waiting).
#define SYSCALL_INSTR_LEN 7
static void rewind_syscall(Context *ctx) {
#ifdef __ARCH_NATIVE
  ctx->uc.uc_mcontext.gregs[REG_RIP] -= SYSCALL_INSTR_LEN;
#else
  ctx->rip -= SYSCALL_INSTR_LEN;
#endif
}

// Time base: count timer interrupts (100 Hz on the native backend) rather than
// reading the AM timer device -- the latter would lazily initialise the SDL
// I/O stack, which aborts the machine (SDL_QUIT -> halt) on a headless host.
#define TICK_HZ 100
static volatile uint64_t g_ticks = 0;
static uint64_t uptime_us(void) { return g_ticks * (1000000ULL / TICK_HZ); }

// Basic sanity check that a user pointer/range is inside user space.
static int user_ok(uintptr_t va, size_t n) {
  return va >= USER_BASE && va + n <= USER_END && va + n >= va;
}

// ------------------------------------------------------------------ syscalls
static int sys_kputc(task_t *p, char ch) {
  (void)p; putch(ch); return 0;
}

static int sys_getpid(task_t *p) { return p->pid; }

static int64_t sys_uptime(task_t *p) { (void)p; return (int64_t)(uptime_us() / 1000); } // ms

static void *sys_mmap(task_t *p, void *addr, int length, int prot, int flags) {
  if (length <= 0) return (void *)-1;
  int amprot = 0;
  if (prot & PROT_READ)  amprot |= MMAP_READ;
  if (prot & PROT_WRITE) amprot |= MMAP_WRITE;
  if (amprot == 0) amprot = MMAP_READ;

  uintptr_t base = addr ? (uintptr_t)addr : p->heap_top;
  base = base & ~(uintptr_t)(PGSIZE - 1);
  unsigned npg = ((unsigned)length + PGSIZE - 1) / PGSIZE;

  if (flags == MAP_UNMAP) return (void *)-1;  // unmap unsupported (bounded demo)

  for (unsigned i = 0; i < npg; i++) {
    void *pa = pmm->alloc(PGSIZE);
    if (!pa) return (void *)-1;
    memset(pa, 0, PGSIZE);
    up_map(p, base + (uintptr_t)i * PGSIZE, pa, amprot);
  }
  if (!addr) p->heap_top = base + (uintptr_t)npg * PGSIZE;
  return (void *)base;
}

// fork(): deep-copy the parent address space and its trap frame.
static int sys_fork(task_t *parent, Context *ctx) {
  task_t *child = proc_alloc(parent->name);
  child->parent = parent;

  for (int i = 0; i < parent->nmap; i++) {
    struct vmap *m = &parent->maps[i];
    void *pa = pmm->alloc(PGSIZE);
    panic_on(!pa, "fork: out of memory");
    memcpy(pa, m->pa, PGSIZE);              // parent page is mapped now -> readable
    up_map(child, m->va, pa, m->prot);
  }
  child->heap_top = parent->heap_top;
  if (vfs_fork) vfs_fork(parent, child);

  // Child resumes exactly like the parent, but fork() returns 0 in the child.
  Context *cc = (Context *)(child->stack + STACK_SZ) - 1;
  *cc = *ctx;
  cc->GPRx = 0;
#ifdef __ARCH_NATIVE
  cc->vm_head = child->as.ptr;
  cc->ksp     = (uintptr_t)(child->stack + STACK_SZ);
#else
  cc->cr3  = child->as.ptr;
  cc->rsp0 = (uintptr_t)(child->stack + STACK_SZ);
#endif
  child->context = cc;

  ptable_add(child);
  kmt_register(child);
  return child->pid;   // parent's return value
}

// exec(): replace the current image with another embedded program (or a vfs
// file if L4 provides it).  Old physical pages are intentionally leaked (the
// bounded demo never runs enough exec()s to matter; documented in README).
static int sys_exec(task_t *p, const char *path, char *const argv[]) {
  (void)argv;
  if (!user_ok((uintptr_t)path, 1)) return -1;
  const char *name = path;
  if (name[0] == '/') { name++; if (name[0]=='b'&&name[1]=='i'&&name[2]=='n'&&name[3]=='/') name += 4; }
  const uprog_t *up = uprog_find(name);
  if (!up) return -1;

  // Fresh address space for the new image.
  AddrSpace  old_as   = p->as;
  struct vmap *old_maps = p->maps;
  int          old_nmap = p->nmap;

  p->maps = pmm->alloc(sizeof(struct vmap) * PROC_MAXMAP);
  p->nmap = 0;
  p->heap_top = USER_HEAP;
  protect(&p->as);
  uintptr_t entry = proc_load_image(p, up->bin, up->len);

  Area kstack = { p->stack, p->stack + STACK_SZ };
  p->context = ucontext(&p->as, kstack, (void *)entry);

  (void)old_as; (void)old_maps; (void)old_nmap;   // leaked (see note above)
  return 0;   // ignored: we return the new user context from the trap handler
}

// Reparent a dying process' children to init, so wait() still works.
static void reparent_children(task_t *dead) {
  kmt->spin_lock(&plock);
  for (int i = 0; i < nptable; i++)
    if (ptable[i]->parent == dead) ptable[i]->parent = initproc;
  kmt->spin_unlock(&plock);
}

static void sys_exit(task_t *p, int status) {
  if (vfs_close_all) vfs_close_all(p);
  // init exiting means the workload is done: shut the machine down cleanly so
  // batch runs terminate (and produce a captured exit code) instead of idling.
  if (p == initproc) {
    printf("init: exited with status %d -- halting\n", status);
    halt(status);
  }
  p->exit_status = status;
  p->xstate = 1;                 // zombie
  reparent_children(p);
  // wake the parent if it is blocked in wait()
  if (p->parent && p->parent->waiting) {
    p->parent->waiting = 0;
    p->parent->state   = ST_RUNNABLE;
  }
  p->state = ST_BLOCKED;         // never scheduled again; reaped by wait()
}

// Reap one zombie child if available.  Return values:
//    >=0  reaped that pid
//    -1   the process has no children
//    -2   children exist but none has exited yet (caller should block)
//    -3   a child has exited but is still transiently on a CPU stack (deferred
//         SMP release); caller should yield and retry rather than block.
#define WAIT_NONE   (-1)
#define WAIT_BLOCK  (-2)
#define WAIT_RETRY  (-3)
static int try_reap(task_t *p, int *status_va) {
  int have_child = 0, zombie_pending = 0, reaped = -1;
  kmt->spin_lock(&plock);
  for (int i = 0; i < nptable; i++) {
    task_t *c = ptable[i];
    if (c->parent != p) continue;
    have_child = 1;
    if (c->xstate && c->on_cpu == 0) {   // exited and off every CPU stack
      reaped = c->pid;
      if (status_va) *status_va = c->exit_status;
      ptable[i] = ptable[--nptable];     // remove from table
      kmt->spin_unlock(&plock);
      kmt->teardown(c);                  // unlink from scheduler list
      proc_free_mem(c);
      pmm->free(c);
      return reaped;
    }
    if (c->xstate) zombie_pending = 1;   // exited, but on_cpu still set
  }
  kmt->spin_unlock(&plock);
  if (zombie_pending) return WAIT_RETRY;
  return have_child ? WAIT_BLOCK : WAIT_NONE;
}

static int sys_kill(task_t *p, int pid) {
  task_t *target = NULL;
  kmt->spin_lock(&plock);
  for (int i = 0; i < nptable; i++)
    if (ptable[i]->pid == pid) { target = ptable[i]; break; }
  kmt->spin_unlock(&plock);
  if (!target || target == p) return -1;
  // smp=1 demo: target is not currently running, so mark it a zombie directly.
  if (vfs_close_all) vfs_close_all(target);
  target->exit_status = -1;
  target->xstate = 1;
  reparent_children(target);
  if (target->parent && target->parent->waiting) {
    target->parent->waiting = 0;
    target->parent->state = ST_RUNNABLE;
  }
  target->state = ST_BLOCKED;
  return 0;
}

// ------------------------------------------------------------ syscall dispatch
// Returns 1 if the current task must be rescheduled (blocked/exited), else 0.
static int do_syscall(task_t *p, Context *ctx, int *resched) {
  long num = ctx->GPRx;
  long a0 = ctx->GPR1, a1 = ctx->GPR2, a2 = ctx->GPR3, a3 = ctx->GPR4;
  long ret = -1;
  *resched = 0;

  switch (num) {
    case SYS_kputc:  ret = sys_kputc(p, (char)a0); break;
    case SYS_getpid: ret = sys_getpid(p); break;
    case SYS_uptime: ret = sys_uptime(p); break;
    case SYS_mmap:   ret = (long)sys_mmap(p, (void*)a0, (int)a1, (int)a2, (int)a3); break;
    case SYS_fork:   ret = sys_fork(p, ctx); break;
    case SYS_kill:   ret = sys_kill(p, (int)a0); break;

    case SYS_exec:
      if (sys_exec(p, (const char*)a0, (char* const*)a1) == 0) {
        *resched = 1;   // switch to the freshly-built user context
        return 0;
      }
      ret = -1; break;

    case SYS_exit:
      sys_exit(p, (int)a0);
      *resched = 1;
      return 0;

    case SYS_wait: {
      int st = 0;
      int r = try_reap(p, (a0 && user_ok(a0, sizeof(int))) ? &st : NULL);
      if (r == WAIT_BLOCK) {         // children exist, none exited: block
        p->waiting = 1;
        p->state = ST_BLOCKED;
        rewind_syscall(ctx);
        *resched = 1;
        return 0;
      }
      if (r == WAIT_RETRY) {         // a child exited but is still on-stack: yield
        rewind_syscall(ctx);
        *resched = 1;               // stay RUNNABLE and retry next schedule
        return 0;
      }
      if (r >= 0 && a0 && user_ok(a0, sizeof(int))) *(int*)a0 = st;
      ret = r;
      break;
    }

    case SYS_sleep: {
      uint64_t now = uptime_us();
      if (!p->sleeping) {
        p->sleeping = 1;
        p->wake_us  = now + (uint64_t)(int)a0 * 1000000ULL;
        p->state = ST_BLOCKED;
        rewind_syscall(ctx);
        *resched = 1;
        return 0;
      }
      if (now >= p->wake_us) { p->sleeping = 0; ret = 0; break; }
      p->state = ST_BLOCKED;         // spurious wake: block again
      rewind_syscall(ctx);
      *resched = 1;
      return 0;
    }

    // ---- file system calls: serviced by L4 vfs when present -----------------
    case SYS_open:  ret = vfs_open  ? vfs_open(p, (const char*)a0, (int)a1) : -1; break;
    case SYS_read:  ret = vfs_read  ? vfs_read(p, (int)a0, (void*)a1, (int)a2) : -1; break;
    case SYS_write:
      if (vfs_write) ret = vfs_write(p, (int)a0, (const void*)a1, (int)a2);
      else { const char *b = (const char*)a1; for (int i=0;i<(int)a2;i++) putch(b[i]); ret = a2; }
      break;
    case SYS_close: ret = vfs_close ? vfs_close(p, (int)a0) : -1; break;
    case SYS_lseek: ret = vfs_lseek ? vfs_lseek(p, (int)a0, (int)a1, (int)a2) : -1; break;
    case SYS_fstat: ret = vfs_fstat ? vfs_fstat(p, (int)a0, (void*)a1) : -1; break;
    case SYS_dup:   ret = vfs_dup   ? vfs_dup(p, (int)a0) : -1; break;
    case SYS_mkdir: ret = vfs_mkdir ? vfs_mkdir(p, (const char*)a0) : -1; break;
    case SYS_link:  ret = vfs_link  ? vfs_link(p, (const char*)a0, (const char*)a1) : -1; break;
    case SYS_unlink:ret = vfs_unlink? vfs_unlink(p, (const char*)a0) : -1; break;
    case SYS_chdir: ret = vfs_chdir ? vfs_chdir(p, (const char*)a0) : -1; break;

    default:
      printf("uproc: pid %d unknown syscall %d\n", p->pid, (int)num);
      ret = -1; break;
  }

  ctx->GPRx = ret;
  return 0;
}

// ------------------------------------------------------------ trap handlers
static Context *uproc_syscall(Event ev, Context *ctx) {
  task_t *p = kmt_current();
  if (!p || !p->is_user) return NULL;   // not a user process; ignore
  int resched = 0;
  do_syscall(p, ctx, &resched);
  if (resched) return kmt_schedule(ev, ctx);
  return ctx;   // resume the same process with GPRx set
}

// Wake sleepers whose deadline has passed (runs on every timer tick).
static Context *uproc_tick(Event ev, Context *ctx) {
  (void)ev; (void)ctx;
  g_ticks++;
  uint64_t now = uptime_us();
  kmt->spin_lock(&plock);
  for (int i = 0; i < nptable; i++) {
    task_t *t = ptable[i];
    if (t->is_user && t->sleeping && t->state == ST_BLOCKED && now >= t->wake_us)
      t->state = ST_RUNNABLE;
  }
  kmt->spin_unlock(&plock);
  return NULL;   // never chooses a context; the scheduler does
}

// A user page fault that we do not handle kills the offending process.
static Context *uproc_pagefault(Event ev, Context *ctx) {
  task_t *p = kmt_current();
  if (!p || !p->is_user) return NULL;
  printf("uproc: pid %d segfault at %p (cause %d) -- killed\n",
         p->pid, (void *)ev.ref, (int)ev.cause);
  sys_exit(p, -1);
  return kmt_schedule(ev, ctx);
}

// ------------------------------------------------------------------- module
static void uproc_init(void) {
  kmt->spin_init(&plock, "uproc");
  next_pid = 1; nptable = 0;
  vme_init(up_pgalloc, up_pgfree);

  if (vfs_init) vfs_init();   // L4: mount ramfs/procfs/devfs, populate /bin

  // Dispatch: syscalls + timer wakeups + page faults.  Timer scheduling is
  // installed by kmt at INT32_MIN; our tick runs alongside it.
  os->on_irq(0,         EVENT_SYSCALL,   uproc_syscall);
  os->on_irq(0,         EVENT_PAGEFAULT, uproc_pagefault);
  os->on_irq(INT32_MIN, EVENT_IRQ_TIMER, uproc_tick);

  const uprog_t *up = uprog_find("init");
  panic_on(!up, "uproc: no embedded 'init' program");
  initproc = proc_spawn("init", up->bin, up->len);
  printf("uproc: created init (pid %d), entering user space...\n", initproc->pid);
}

// L3 exposes the raw process operations too (used by tests / kernel callers).
static int   up_kputc(task_t *t, char ch)                 { return sys_kputc(t, ch); }
static int   up_fork (task_t *t)                          { return sys_fork(t, t->context); }
static int   up_wait (task_t *t, int *status)             { return try_reap(t, status); }
static int   up_exit (task_t *t, int status)              { sys_exit(t, status); return 0; }
static int   up_kill (task_t *t, int pid)                 { return sys_kill(t, pid); }
static void *up_mmap (task_t *t, void *a, int l, int pr, int fl) { return sys_mmap(t, a, l, pr, fl); }
static int   up_getpid(task_t *t)                         { return sys_getpid(t); }
static int   up_sleep(task_t *t, int s)                   { (void)t; (void)s; return 0; }
static int64_t up_uptime(task_t *t)                       { return sys_uptime(t); }

MODULE_DEF(uproc) = {
  .init   = uproc_init,
  .kputc  = up_kputc,
  .fork   = up_fork,
  .wait   = up_wait,
  .exit   = up_exit,
  .kill   = up_kill,
  .mmap   = up_mmap,
  .getpid = up_getpid,
  .sleep  = up_sleep,
  .uptime = up_uptime,
};
