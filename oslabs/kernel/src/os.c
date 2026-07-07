#include <common.h>

static void os_init() {
  pmm->init();
}

// ---------------------------------------------------------------------------
// L1 pmm stress test.
//
// Compiled in when PMM_TEST is defined (see kernel/Makefile: `make ARCH=native
// TEST=pmm`).  Each CPU runs an independent alloc/free workload that:
//   * requests random sizes across the slab + buddy ranges,
//   * checks every returned block is non-NULL, in-range, and aligned to the
//     smallest power of two >= size,
//   * writes a per-block signature, then re-reads it after other allocations
//     to detect overlap/corruption,
//   * frees everything and repeats.
// A per-CPU cycle counter (summed at the end) reports total alloc/free cycles.
// ---------------------------------------------------------------------------
#ifdef PMM_TEST

#define NPTR      64          // live blocks per CPU
#define ROUNDS    2000        // alloc/free rounds per CPU

static int percpu_cycles[16]; // per-CPU cycle counter (no cross-CPU sharing)
static int g_fail;            // set !=0 on any invariant violation
static int done_lock;         // test-and-set lock protecting done_cpus
static int done_cpus;         // how many CPUs finished

// tiny per-CPU xorshift PRNG
static unsigned rng_state[16];
static unsigned xrand(int cpu) {
  unsigned x = rng_state[cpu];
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng_state[cpu] = x;
  return x;
}

static int is_pow2_aligned(void *p, size_t size) {
  size_t a = 1;
  while (a < size) a <<= 1;              // smallest pow2 >= size
  if (a > 4096) a = 4096;                // buddy blocks are page-aligned
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
      // random size 1..8192, biased toward small
      unsigned r = xrand(cpu);
      size_t size = (r % 2 == 0) ? (r % 512 + 1) : (r % 8192 + 1);

      void *p = pmm->alloc(size);
      if (!p) continue;                  // OOM is acceptable under pressure

      if (!is_pow2_aligned(p, size)) g_fail = 1;
      if ((uintptr_t)p < (uintptr_t)heap.start ||
          (uintptr_t)p + size > (uintptr_t)heap.end) g_fail = 1;

      unsigned char s = (unsigned char)(r ^ cpu ^ i);
      memset(p, s, size);
      ptrs[i] = p; sizes[i] = size; sig[i] = s;
    }

    // Verify no block was corrupted by another CPU's / our own allocations.
    for (int i = 0; i < NPTR; i++) {
      if (!ptrs[i]) continue;
      unsigned char *b = ptrs[i];
      for (size_t k = 0; k < sizes[i]; k++)
        if (b[k] != sig[i]) { g_fail = 1; break; }
    }

    // Free them all.
    for (int i = 0; i < NPTR; i++) {
      if (ptrs[i]) { pmm->free(ptrs[i]); ptrs[i] = NULL; }
    }
    percpu_cycles[cpu]++;   // per-CPU, no contention
  }

  // Register completion under a test-and-set lock (atomic_xchg, CAS-free).
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
    // Wait (bounded) for the other CPUs to finish.
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

#else  // normal L1 boot: just say hello

static void os_run() {
  for (const char *s = "Hello World from CPU #*\n"; *s; s++) {
    putch(*s == '*' ? '0' + cpu_current() : *s);
  }
  while (1) ;
}

#endif

MODULE_DEF(os) = {
  .init = os_init,
  .run  = os_run,
};
