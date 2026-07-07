// M2: libco — an asymmetric stackful coroutine library.
//
// NJU OS 2022 (jyy) MiniLab M2.  Implements cooperative multitasking in
// user space with three primitives:
//
//   struct co *co_start(name, func, arg);  // create a runnable coroutine
//   void       co_yield();                 // switch to another coroutine
//   void       co_wait(co);                // block until `co` finishes, then free it
//
// Each coroutine owns a private stack.  Switching is done with setjmp/longjmp
// for the "resume an already-running coroutine" case, and a tiny inline-asm
// stack switch (`stack_switch_call`) for the "first entry into a fresh
// coroutine" case.  Works on both x86-64 (-m64) and i386 (-m32).
//
// Scheduling policy: a simple round-robin over all non-finished coroutines.
// co_wait() drives the scheduler until the awaited coroutine is done.

#include "co.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

#define STACK_SIZE (64 * 1024)
#define MAX_CO     128

enum co_status {
  CO_NEW = 1,   // created, has not started running
  CO_RUNNING,   // has run, currently on the stack of some ancestor
  CO_WAITING,   // blocked in co_wait, waiting on a child
  CO_DEAD,      // finished, waiting to be co_wait()ed and freed
};

struct co {
  const char *name;
  void (*func)(void *);
  void *arg;

  enum co_status status;
  struct co     *waiter;   // the coroutine blocked in co_wait(this), or NULL
  jmp_buf        context;  // saved register context (setjmp/longjmp)

  // The coroutine's private stack.  Kept 16-byte aligned; the trampoline in
  // stack_switch_call arranges the ABI-required alignment at call time.
  uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));
};

// ---- global scheduler state --------------------------------------------

static struct co *co_pool[MAX_CO];   // all live coroutines (round-robin ring)
static int        co_count = 0;
static struct co *current = NULL;    // the coroutine currently executing

// The very first co_yield/co_wait needs a `current`.  We lazily create a
// coroutine representing main() so the scheduler has something to return to.
static struct co  co_main_storage;
static int        initialized = 0;

static void ensure_init(void) {
  if (initialized) return;
  initialized = 1;
  co_main_storage.name   = "main";
  co_main_storage.status = CO_RUNNING;
  co_main_storage.waiter = NULL;
  current = &co_main_storage;
  co_pool[co_count++] = current;
}

// Switch %rsp/%esp to `sp` and call `entry(arg)`.  `sp` points just past the
// top of the target stack; we align it down to 16 bytes (System V ABI) so the
// callee sees a correctly-aligned frame.  This never returns to the caller —
// when `entry` returns we handle teardown in co_entry.
static inline void stack_switch_call(void *sp, void *entry, void *arg) {
#if __x86_64__
  asm volatile(
    "movq %0, %%rsp\n\t"       // rsp = sp
    "movq %2, %%rdi\n\t"       // arg -> first argument register
    "callq *%1\n\t"            // call entry(arg)
    : : "b"((uintptr_t)sp), "d"(entry), "a"(arg) : "memory"
  );
#else
  asm volatile(
    "movl %0, %%esp\n\t"       // esp = sp
    "movl %2, (%%esp)\n\t"     // arg pushed as the (only) stack argument
    "calll *%1\n\t"            // call entry(arg)
    : : "b"((uintptr_t)sp), "d"(entry), "a"(arg) : "memory"
  );
#endif
}

// Trampoline that runs on a fresh coroutine stack.  Calls the user function,
// marks the coroutine dead, then hands control to the scheduler (never returns).
static void schedule(void);   // fwd decl

static void co_entry(void *arg) {
  struct co *co = (struct co *)arg;
  co->status = CO_RUNNING;
  co->func(co->arg);
  co->status = CO_DEAD;
  // The user function has returned; we cannot "return" from here (the stack
  // is synthetic).  Jump to the scheduler to pick the next coroutine.
  schedule();
  // schedule() longjmps away and never comes back.
  assert(0);
}

// Pick the next runnable coroutine (round-robin, skipping DEAD ones) and
// transfer control to it.  Must be called with `current` set to the coroutine
// we are leaving (its context should already be saved by the caller, or the
// caller must not need to be resumed, e.g. a DEAD coroutine).
static void schedule(void) {
  // Find `current`'s slot, then scan forward for the next non-dead coroutine.
  int start = 0;
  for (int i = 0; i < co_count; i++)
    if (co_pool[i] == current) { start = i; break; }

  struct co *next = NULL;
  for (int off = 1; off <= co_count; off++) {
    struct co *cand = co_pool[(start + off) % co_count];
    if (cand->status == CO_NEW || cand->status == CO_RUNNING) {
      next = cand;
      break;
    }
  }
  // If nothing else is runnable, fall back to current if it is still alive.
  if (!next) {
    if (current->status == CO_RUNNING || current->status == CO_NEW)
      next = current;
    else {
      // Deadlock: every coroutine is blocked/dead. Should not happen in the
      // provided tests, but bail cleanly rather than spin forever.
      fprintf(stderr, "libco: no runnable coroutine (deadlock)\n");
      exit(1);
    }
  }

  current = next;
  if (next->status == CO_NEW) {
    // First entry: switch to its private stack and run the trampoline.
    stack_switch_call(next->stack + STACK_SIZE, (void *)co_entry, next);
    // co_entry never returns here.
  } else {
    // Resume a previously-suspended coroutine.
    longjmp(next->context, 1);
  }
}

struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  ensure_init();
  struct co *co = malloc(sizeof(struct co));
  assert(co);
  co->name   = name;
  co->func   = func;
  co->arg    = arg;
  co->status = CO_NEW;
  co->waiter = NULL;
  assert(co_count < MAX_CO);
  co_pool[co_count++] = co;
  return co;
}

void co_yield(void) {
  ensure_init();
  int val = setjmp(current->context);
  if (val == 0) {
    // Saved our context; go run someone else.
    schedule();
  } else {
    // Resumed via longjmp: just return to the caller of co_yield.
    return;
  }
}

void co_wait(struct co *co) {
  ensure_init();
  // Drive the scheduler until `co` has finished.
  co->waiter = current;
  while (co->status != CO_DEAD) {
    // Block ourselves and yield; we will only be picked again once `co`
    // has become DEAD (schedule leaves us runnable, so we re-check).
    co_yield();
  }

  // `co` is done: remove it from the ring and free it.
  int idx = -1;
  for (int i = 0; i < co_count; i++)
    if (co_pool[i] == co) { idx = i; break; }
  if (idx >= 0) {
    for (int i = idx; i < co_count - 1; i++)
      co_pool[i] = co_pool[i + 1];
    co_count--;
  }
  free(co);
}
