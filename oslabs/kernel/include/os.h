#include <common.h>

#define MAX_CPU  16
#define STACK_SZ (16 * 4096)   // 64 KiB kernel stack per task

// ---- task ----------------------------------------------------------------

enum task_state {
  ST_UNUSED = 0,
  ST_RUNNABLE,   // ready to run / currently running
  ST_BLOCKED,    // waiting on a semaphore
  ST_DEAD,       // finished, awaiting teardown
};

struct task {
  const char     *name;
  Context        *context;     // saved register context (points into stack)
  enum task_state state;
  int             cpu;         // which CPU is currently running it (-1 if none)
  struct task    *sched_next;  // scheduler ready-list link
  struct task    *sem_next;    // semaphore wait-queue link
  // Interrupt-disable nesting saved with the task so it survives migration to
  // another CPU across a context switch (xv6 push_off/pop_off semantics).
  int             saved_ncli;
  int             saved_intena;
  // 1 while some CPU is still executing on this task's kernel stack (running it
  // or unwinding the scheduler on it).  A task must NOT be picked by another CPU
  // until its previous CPU has fully switched away, or the two would corrupt the
  // shared stack.  Cleared by the scheduler on the *next* switch (deferred).
  volatile int    on_cpu;
  uint8_t         stack[STACK_SZ] __attribute__((aligned(16)));
};

// ---- spinlock ------------------------------------------------------------

struct spinlock {
  int         locked;      // 0/1 test-and-set flag
  const char *name;
  int         cpu;         // holder CPU (for debugging / assertions)
};

// ---- semaphore -----------------------------------------------------------

struct semaphore {
  int             value;
  const char     *name;
  struct spinlock lock;    // protects value + wait queue
  struct task    *waiters; // FIFO queue of blocked tasks (via sem_next)
};
