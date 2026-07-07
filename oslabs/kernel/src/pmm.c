// L1: pmm — a concurrent (SMP-safe) physical memory allocator.
//
// NJU OS 2022 (jyy) OSLab 1.  Implements pmm->alloc(size) / pmm->free(ptr)
// over the AbstractMachine `heap` region, correct under concurrent access from
// up to MAX_CPU processors.
//
// Requirements met:
//   * alloc(size) returns a block of >= size bytes, aligned to the smallest
//     power of two >= size (so 4 KiB requests are page-aligned, etc.).
//   * free(ptr) returns a previously-allocated block; ptr==NULL is ignored.
//   * thread-safe on SMP: small allocations use per-CPU slab caches (mostly
//     lock-free w.r.t. other CPUs); page/large allocations use a global
//     spinlocked buddy allocator.
//
// Layout
// ------
//   [heap.start ................................................ heap.end)
//    page-frame metadata table         |   PAGE-aligned data region
//
// The data region is carved into PAGE_SIZE (4 KiB) frames managed by a buddy
// allocator (orders 0..MAX_ORDER, block = PAGE<<order).  Small objects
// (<= PAGE/2) are served from per-CPU slabs: each slab is one page split into
// equal cells of a power-of-two size class; a page-descriptor table records
// which slab/buddy block a pointer belongs to so free() is O(1).

#include <common.h>

#define PAGE_SIZE   4096
#define MAX_ORDER   16                         // up to 4096 << 16 = 256 MiB block
#define MAX_CPU     16

// ----------------------------------------------------------------- spinlock

typedef struct {
  int locked;
  const char *name;
} pmm_lock_t;

static void lock_init(pmm_lock_t *lk, const char *name) {
  lk->locked = 0;
  lk->name = name;
}
static void lock(pmm_lock_t *lk) {
  while (atomic_xchg(&lk->locked, 1)) ;    // test-and-set spin
}
static void unlock(pmm_lock_t *lk) {
  atomic_xchg(&lk->locked, 0);
}

// ------------------------------------------------------------ buddy allocator

// Each free block of order k is linked through its first 8 bytes.
typedef struct free_block {
  struct free_block *next;
} free_block_t;

static free_block_t *free_lists[MAX_ORDER + 1];
static pmm_lock_t     buddy_lock;

static uintptr_t data_start, data_end;   // PAGE-aligned data region
static size_t    total_pages;

// Round `n` up to a power of two.
static size_t next_pow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Smallest order whose block (PAGE<<order) >= size.
static int size_to_order(size_t size) {
  size_t need = size < PAGE_SIZE ? PAGE_SIZE : size;
  int order = 0;
  size_t block = PAGE_SIZE;
  while (block < need && order < MAX_ORDER) { block <<= 1; order++; }
  return order;
}

static void buddy_push(int order, void *blk) {
  free_block_t *b = (free_block_t *)blk;
  b->next = free_lists[order];
  free_lists[order] = b;
}

static void *buddy_pop(int order) {
  free_block_t *b = free_lists[order];
  if (b) free_lists[order] = b->next;
  return b;
}

// Allocate a block of the given order, splitting larger blocks as needed.
// Caller holds buddy_lock.
static void *buddy_alloc(int order) {
  int k = order;
  while (k <= MAX_ORDER && !free_lists[k]) k++;
  if (k > MAX_ORDER) return NULL;            // out of memory

  void *blk = buddy_pop(k);
  // Split down to the requested order, returning the buddies to free lists.
  while (k > order) {
    k--;
    size_t half = (size_t)PAGE_SIZE << k;
    void *buddy = (char *)blk + half;
    buddy_push(k, buddy);
  }
  return blk;
}

// Return a block of `order` to the free lists, coalescing with its buddy when
// possible.  Caller holds buddy_lock.
static void buddy_free(int order, void *blk) {
  uintptr_t addr = (uintptr_t)blk;
  while (order < MAX_ORDER) {
    size_t bsize = (size_t)PAGE_SIZE << order;
    uintptr_t buddy = ((addr - data_start) ^ bsize) + data_start;
    // Search this order's list for the buddy; if present, coalesce.
    free_block_t **pp = &free_lists[order];
    free_block_t *found = NULL;
    while (*pp) {
      if ((uintptr_t)(*pp) == buddy) { found = *pp; *pp = (*pp)->next; break; }
      pp = &(*pp)->next;
    }
    if (!found) break;                        // buddy not free -> stop merging
    if (buddy < addr) addr = buddy;           // merged block starts at lower addr
    order++;
  }
  buddy_push(order, (void *)addr);
}

// --------------------------------------------------------------- page headers

// Every data page has a descriptor telling free() what kind of block starts
// there.  Descriptors live in a table at the front of the heap.
typedef enum { PG_FREE = 0, PG_BUDDY, PG_SLAB } pgkind_t;

typedef struct {
  pgkind_t kind;
  int      order;        // for PG_BUDDY: block order
  int      obj_size;     // for PG_SLAB: cell size
} page_desc_t;

static page_desc_t *pgtab;

static page_desc_t *desc_of(void *ptr) {
  size_t idx = ((uintptr_t)ptr - data_start) / PAGE_SIZE;
  return &pgtab[idx];
}

// --------------------------------------------------------- per-CPU slab caches

// Size classes: 8,16,32,...,2048 bytes (<= PAGE/2).  Each CPU keeps a free
// list per class; refills come from the (locked) buddy page allocator.
#define NCLASS 9   // 8<<0 .. 8<<8 = 8..2048
static const int class_size[NCLASS] = {8,16,32,64,128,256,512,1024,2048};

typedef struct {
  free_block_t *free[NCLASS];
} cpu_cache_t;

static cpu_cache_t caches[MAX_CPU];

static int size_to_class(size_t size) {
  size_t s = next_pow2(size < 8 ? 8 : size);
  for (int i = 0; i < NCLASS; i++)
    if ((size_t)class_size[i] >= s) return i;
  return -1;    // too big for a slab
}

// Grab one page from the buddy allocator and carve it into `cell` slots of the
// given size class, threading them onto this CPU's free list.
static void slab_refill(cpu_cache_t *cc, int cls) {
  lock(&buddy_lock);
  void *page = buddy_alloc(0);   // one 4 KiB page
  unlock(&buddy_lock);
  if (!page) return;

  desc_of(page)->kind     = PG_SLAB;
  desc_of(page)->obj_size = class_size[cls];

  int cell = class_size[cls];
  int n = PAGE_SIZE / cell;
  char *p = (char *)page;
  for (int i = 0; i < n; i++) {
    free_block_t *b = (free_block_t *)(p + i * cell);
    b->next = cc->free[cls];
    cc->free[cls] = b;
  }
}

// ------------------------------------------------------------------- pmm API

static void *kalloc(size_t size) {
  if (size == 0) return NULL;

  int cls = size_to_class(size);
  if (cls >= 0) {
    // Small allocation from this CPU's slab (the per-CPU list is not shared
    // across processors, so no lock is needed on the fast path).
    int cpu = cpu_current();
    cpu_cache_t *cc = &caches[cpu];
    if (!cc->free[cls]) slab_refill(cc, cls);
    free_block_t *b = cc->free[cls];
    if (!b) return NULL;
    cc->free[cls] = b->next;
    return b;
  }

  // Large allocation: whole buddy blocks, page-descriptor tagged.
  int order = size_to_order(size);
  lock(&buddy_lock);
  void *blk = buddy_alloc(order);
  if (blk) { desc_of(blk)->kind = PG_BUDDY; desc_of(blk)->order = order; }
  unlock(&buddy_lock);
  return blk;
}

static void kfree(void *ptr) {
  if (!ptr) return;
  // The block always begins at a page boundary for buddy blocks; for slab
  // objects, round down to the page to find the descriptor.
  void *page = (void *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
  page_desc_t *d = desc_of(page);

  if (d->kind == PG_SLAB) {
    int cls = size_to_class(d->obj_size);
    int cpu = cpu_current();
    free_block_t *b = (free_block_t *)ptr;
    b->next = caches[cpu].free[cls];
    caches[cpu].free[cls] = b;
    return;
  }

  if (d->kind == PG_BUDDY) {
    lock(&buddy_lock);
    d->kind = PG_FREE;
    buddy_free(d->order, ptr);
    unlock(&buddy_lock);
    return;
  }
  // Unknown / double free -> ignore defensively (should not happen).
}

static void pmm_init() {
  uintptr_t pmsize = ((uintptr_t)heap.end - (uintptr_t)heap.start);
  printf("Got %d MiB heap: [%p, %p)\n", pmsize >> 20, heap.start, heap.end);

  lock_init(&buddy_lock, "buddy");
  for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = NULL;
  for (int c = 0; c < MAX_CPU; c++)
    for (int i = 0; i < NCLASS; i++) caches[c].free[i] = NULL;

  // Reserve the front of the heap for the page-descriptor table.
  uintptr_t start = (uintptr_t)heap.start;
  uintptr_t end   = (uintptr_t)heap.end;
  size_t pages_est = (end - start) / PAGE_SIZE;
  size_t tab_bytes = pages_est * sizeof(page_desc_t);

  pgtab = (page_desc_t *)start;
  uintptr_t ds = start + tab_bytes;
  ds = (ds + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);   // align up
  data_start = ds;
  data_end   = end & ~(uintptr_t)(PAGE_SIZE - 1);
  total_pages = (data_end - data_start) / PAGE_SIZE;

  for (size_t i = 0; i < total_pages; i++) pgtab[i].kind = PG_FREE;

  // Seed the buddy free lists: repeatedly carve the largest aligned power-of-two
  // block that fits into the remaining region.
  uintptr_t cur = data_start;
  while (cur < data_end) {
    int order = MAX_ORDER;
    while (order > 0) {
      size_t bsize = (size_t)PAGE_SIZE << order;
      if (cur + bsize <= data_end &&
          ((cur - data_start) % bsize) == 0) break;
      order--;
    }
    buddy_push(order, (void *)cur);
    cur += (size_t)PAGE_SIZE << order;
  }
  printf("pmm: %d data pages, region [%p, %p)\n",
         (int)total_pages, (void *)data_start, (void *)data_end);
}

MODULE_DEF(pmm) = {
  .init  = pmm_init,
  .alloc = kalloc,
  .free  = kfree,
};
