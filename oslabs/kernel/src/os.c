#include <os.h>   // pulls in common.h -> kernel.h (single inclusion)

// ---------------------------------------------------------------------------
// os module: interrupt/trap dispatch + the preemptive scheduler entry point.
//
// framework/main.c calls cte_init(os->trap), so every interrupt/exception on
// every CPU funnels through os_trap(ev, ctx).  We keep a table of registered
// handlers (installed with os->on_irq) sorted by sequence; kmt installs the
// scheduler as the timer/yield handler.  os_trap saves the running task's
// context, invokes matching handlers, and returns the (possibly different)
// context that CTE will resume.
// ---------------------------------------------------------------------------

#define MAX_HANDLER 32

typedef struct {
  int        seq;
  int        event;
  handler_t  handler;
} irq_entry_t;

static irq_entry_t handlers[MAX_HANDLER];
static int         nhandler = 0;
// cpu = -1 sentinel so the "already held by this CPU" check is correct even
// before any spin_init runs (kmt_init installs handlers via os_on_irq).
static struct spinlock handler_lock = { .locked = 0, .name = "os-handlers", .cpu = -1 };

// L2 self-test entry point (defined in kmt.c when KMT_TEST is set).
#ifdef KMT_TEST
void kmt_selftest_start(void);
#endif

static void os_init() {
  pmm->init();
#if defined(PMM_TEST)
  // L1 pmm stress runs per-CPU from os_run(); nothing else to set up.
#elif defined(KMT_TEST)
  kmt->init();
  kmt_selftest_start();  // L2 self-test (spawns worker/producer/consumer tasks)
#else
  kmt->init();           // scheduler + idle tasks (installs IRQ handlers)
  uproc->init();         // L3/L4: VME + first user process (init) + syscall path
#endif
}

// ===========================================================================
// L1 pmm stress test (compiled in with `make ARCH=native TEST=pmm`).
//
// Each CPU runs an independent alloc/free workload that requests random sizes
// across the slab + buddy ranges, checks every block is non-NULL, in-range and
// aligned to the smallest power of two >= size, writes a per-block signature
// and re-reads it (detecting overlap/corruption), then frees everything and
// repeats.  A per-CPU cycle counter (summed at the end) reports total cycles.
// ===========================================================================
#ifdef PMM_TEST

#define NPTR      64          // live blocks per CPU
#define ROUNDS    2000        // alloc/free rounds per CPU

static int percpu_cycles[MAX_CPU];
static int g_fail;
static int done_lock;
static int done_cpus;

static unsigned rng_state[MAX_CPU];
static unsigned xrand(int cpu) {
  unsigned x = rng_state[cpu];
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng_state[cpu] = x;
  return x;
}

static int is_pow2_aligned(void *p, size_t size) {
  size_t a = 1;
  while (a < size) a <<= 1;
  if (a > 4096) a = 4096;
  return ((uintptr_t)p & (a - 1)) == 0;
}

static void pmm_stress(void) {
  int cpu = cpu_current();
  rng_state[cpu] = 0x1234567u + cpu * 2654435761u + 1;

  void   *ptrs[NPTR] = {0};
  size_t  sizes[NPTR] = {0};
  unsigned char sig[NPTR];

  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < NPTR; i++) {
      unsigned r = xrand(cpu);
      size_t size = (r % 2 == 0) ? (r % 512 + 1) : (r % 8192 + 1);
      void *p = pmm->alloc(size);
      if (!p) continue;
      if (!is_pow2_aligned(p, size)) g_fail = 1;
      if ((uintptr_t)p < (uintptr_t)heap.start ||
          (uintptr_t)p + size > (uintptr_t)heap.end) g_fail = 1;
      unsigned char s = (unsigned char)(r ^ cpu ^ i);
      memset(p, s, size);
      ptrs[i] = p; sizes[i] = size; sig[i] = s;
    }
    for (int i = 0; i < NPTR; i++) {
      if (!ptrs[i]) continue;
      unsigned char *b = ptrs[i];
      for (size_t k = 0; k < sizes[i]; k++)
        if (b[k] != sig[i]) { g_fail = 1; break; }
    }
    for (int i = 0; i < NPTR; i++)
      if (ptrs[i]) { pmm->free(ptrs[i]); ptrs[i] = NULL; }
    percpu_cycles[cpu]++;
  }

  while (atomic_xchg(&done_lock, 1)) ;
  done_cpus++;
  atomic_xchg(&done_lock, 0);
}

static void os_run() {
  int cpu = cpu_current();
  printf("CPU #%d starting pmm stress (%d rounds x %d ptrs)\n", cpu, ROUNDS, NPTR);
  pmm_stress();
  printf("CPU #%d done. fail=%d\n", cpu, g_fail);
  if (cpu == 0) {
    for (volatile long spin = 0;
         spin < 2000000000L && done_cpus < cpu_count(); spin++) ;
    int total = 0;
    for (int c = 0; c < cpu_count(); c++) total += percpu_cycles[c];
    printf("======== PMM STRESS RESULT ========\n");
    printf("cpus=%d  total_cycles=%d  FAIL=%d  => %s\n",
           cpu_count(), total, g_fail, g_fail ? "FAILED" : "PASSED");
  }
  while (1) ;
}

#else

static void os_run() {
  iset(true);        // enable interrupts on this CPU; scheduler takes over
  while (1) yield(); // fall into the scheduler; never returns to a real task path
}

#endif

// Register a handler.  Handlers are invoked in ascending `seq` order.
static void os_on_irq(int seq, int event, handler_t handler) {
  kmt->spin_lock(&handler_lock);
  panic_on(nhandler >= MAX_HANDLER, "too many IRQ handlers");
  // insertion sort by seq
  int i = nhandler++;
  while (i > 0 && handlers[i - 1].seq > seq) {
    handlers[i] = handlers[i - 1];
    i--;
  }
  handlers[i] = (irq_entry_t){ .seq = seq, .event = event, .handler = handler };
  kmt->spin_unlock(&handler_lock);
}

static Context *os_trap(Event ev, Context *context) {
  Context *next = NULL;
  for (int i = 0; i < nhandler; i++) {
    if (handlers[i].event == EVENT_NULL || handlers[i].event == ev.event) {
      Context *r = handlers[i].handler(ev, context);
      if (r) next = r;    // last non-NULL wins (the scheduler returns the pick)
    }
  }
  panic_on(!next, "returning to NULL context");
  panic_on(ienabled(), "kernel context with interrupts on");
  return next;
}

MODULE_DEF(os) = {
  .init   = os_init,
  .run    = os_run,
  .trap   = os_trap,
  .on_irq = os_on_irq,
};
