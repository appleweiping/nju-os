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
  kmt->init();       // sets up the scheduler + idle tasks (installs IRQ handlers)
#ifndef KMT_TEST
  dev->init();       // full device stack (L2/L3); skipped in the isolated test
#else
  kmt_selftest_start();
#endif
}

static void os_run() {
  iset(true);        // enable interrupts on this CPU; scheduler takes over
  while (1) yield(); // fall into the scheduler; never returns to a real task path
}

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
