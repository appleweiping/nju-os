#include <common.h>

#define MAX_CPU  16
#define STACK_SZ (16 * 4096)   // 64 KiB kernel stack per task

// ---- user process (L3/L4) ------------------------------------------------

#define PROC_MAXMAP 1024       // max mapped user pages tracked per process
#define PROC_NOFILE 16         // max open files per process (L4)

// One tracked user-space page mapping (va -> pa, protection).  We keep our own
// list (rather than reading it back from the AM VMHead) so that fork can copy
// the address space and exit/exec can release the physical pages.
struct vmap {
  uintptr_t va;
  void     *pa;
  int       prot;
};

// ---- task ----------------------------------------------------------------

enum task_state {
  ST_UNUSED = 0,
  ST_RUNNABLE,   // ready to run / currently running
  ST_BLOCKED,    // waiting on a semaphore / wait() / sleep()
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

  // ---- user-process fields (L3 uproc / L4 vfs); zero for pure kernel tasks --
  int             is_user;     // 1 if this task runs a user program
  int             pid;         // process id (>0 for user procs)
  AddrSpace       as;          // per-process virtual address space
  struct vmap    *maps;        // tracked user page mappings (PROC_MAXMAP)
  int             nmap;        // number of live mappings
  uintptr_t       heap_top;    // next free user va for mmap()
  struct task    *parent;      // parent process (for wait())
  int             exit_status; // exit() code, read by wait()
  int             xstate;      // 1 = zombie (exited, not yet reaped)
  int             sleeping;    // 1 = blocked in sleep()
  uint64_t        wake_us;     // wake deadline (AM uptime us) when sleeping
  int             waiting;     // 1 = blocked in wait()
  void           *ofile[PROC_NOFILE];  // open file table (struct file*, L4)
  char            cwd[64];     // current working directory (L4)

  uint8_t         stack[STACK_SZ] __attribute__((aligned(16)));
};

// kmt hooks used by uproc (defined in kmt.c)
void      kmt_register(struct task *task);   // add a prebuilt-context task to the ready list
struct task *kmt_current(void);              // task running on this CPU
Context  *kmt_schedule(Event ev, Context *ctx);  // save-current / pick-next / return ctx

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
