/*
 * libnoheap.so (v2) — make heap pointers JS-safe on bionic.
 *
 * Problem: Android 11+ bionic (scudo) tags every heap pointer in the top
 * byte (0xb4...). opentui 0.8 returns such a pointer from ot_context_create
 * and converts it to a JS number in bun:ffi; anything > 2^53 throws
 * "Pointer exceeds safe integer range" and the TUI dies.
 *
 * The clean fix, mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, NONE), exists only
 * since API 31 (Android 12). On Android 11 (API 30) it returns 0 and the tag
 * stays. The allocator also *validates* tags in free/realloc ("Pointer tag
 * ... was truncated"), so pointers cannot simply be untagged.
 *
 * This shim interposes the allocator boundary instead:
 *   - allocations returned to the app are stripped of the tag byte
 *   - pointers passed back to the real allocator (free/realloc/usable_size)
 *     get the tag byte restored, sampled live at startup
 * On Android 12+, mallopt(NONE) succeeds, the sampled tag is 0 and every
 * wrapper degrades to a plain passthrough.
 *
 * Derived from jcz2020/opencode-termux src/noheap.c (mallopt path).
 * Build: clang -shared -fPIC -O2 -o libnoheap.so noheap.c
 * Debug: NOHEAP_DEBUG=1
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int mallopt(int __option, int __value);

#define TAG_MASK 0x00FFFFFFFFFFFFFFULL
/* provisional until the constructor samples the live tag; refined there */
static uintptr_t tag_top = 0xB4ULL << 56;
static int sampled = 0;

static void* (*real_malloc)(size_t);
static void* (*real_calloc)(size_t, size_t);
static void* (*real_realloc)(void*, size_t);
static void* (*real_reallocarray)(void*, size_t, size_t);
static void* (*real_memalign)(size_t, size_t);
static void* (*real_aligned_alloc)(size_t, size_t);
static void* (*real_valloc)(size_t);
static void* (*real_pvalloc)(size_t);
static int (*real_posix_memalign)(void**, size_t, size_t);
static void (*real_free)(void*);
static size_t (*real_malloc_usable_size)(void*);

static volatile int resolving = 0;

/* Pre-resolution fallback arena: only live between process start and the
 * constructor (e.g. allocations inside dlsym itself). Tiny by design. */
static unsigned char arena[1 << 16] __attribute__((aligned(16)));
static volatile size_t arena_off;

static int in_arena(const void* p) {
  return (const unsigned char*)p >= arena && (const unsigned char*)p < arena + sizeof(arena);
}

static void* arena_malloc(size_t n) {
  size_t need = (n + 15) & ~(size_t)15;
  size_t off = __atomic_fetch_add(&arena_off, need + 16, __ATOMIC_RELAXED);
  if (off + need + 16 > sizeof(arena)) return NULL;
  unsigned char* chunk = arena + off;
  *(size_t*)chunk = n;
  return chunk + 16;
}

static size_t arena_usable(void* p) {
  if (!in_arena(p)) return 0;
  return *(size_t*)((unsigned char*)p - 16);
}

static void ensure_resolved(void) {
  if (real_malloc || resolving) return;
  resolving = 1;
  real_malloc = dlsym(RTLD_NEXT, "malloc");
  real_calloc = dlsym(RTLD_NEXT, "calloc");
  real_realloc = dlsym(RTLD_NEXT, "realloc");
  real_reallocarray = dlsym(RTLD_NEXT, "reallocarray");
  real_memalign = dlsym(RTLD_NEXT, "memalign");
  real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
  real_valloc = dlsym(RTLD_NEXT, "valloc");
  real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
  real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
  real_free = dlsym(RTLD_NEXT, "free");
  real_malloc_usable_size = dlsym(RTLD_NEXT, "malloc_usable_size");
  resolving = 0;
}

static void* strip(void* p) { return p ? (void*)((uintptr_t)p & TAG_MASK) : p; }

/* Restore the allocator's tag byte. Pointers that already carry a nonzero
 * top byte (e.g. from non-interposed libc-internal allocations) pass
 * through untouched. */
static void* retag(void* p) {
  uintptr_t u = (uintptr_t)p;
  if (!p || (u >> 56) || !tag_top) return p;
  return (void*)(u | tag_top);
}

__attribute__((constructor))
static void noheap_init(void) {
  ensure_resolved();
  if (!real_malloc) return;

  /* Try the supported API first (Android 12+): if it works the allocator is
   * untagged and every wrapper below becomes a passthrough. */
  int mallopt_rc = mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);

  /* Sample the tag the allocator actually produces right now. */
  void* probe = real_malloc(32);
  if (probe) {
    tag_top = ((uintptr_t)probe >> 56) << 56;
    real_free(probe);
    sampled = 1;
  }

  if (getenv("NOHEAP_DEBUG")) {
    fprintf(stderr,
            "NOHEAP: mallopt=%d sampled_tag=0x%02x mode=%s\n",
            mallopt_rc,
            (unsigned)(tag_top >> 56),
            tag_top ? "interpose (strip/restore)" : "passthrough (untagged heap)");
  }
}

void* malloc(size_t n) {
  ensure_resolved();
  if (!real_malloc) return arena_malloc(n);
  return strip(real_malloc(n));
}

void* calloc(size_t n, size_t s) {
  ensure_resolved();
  if (!real_calloc) {
    void* p = arena_malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
  }
  return strip(real_calloc(n, s));
}

void* realloc(void* p, size_t n) {
  ensure_resolved();
  if (!p) return malloc(n);
  if (in_arena(p)) {
    size_t old = arena_usable(p);
    void* np = malloc(n);
    if (np) memcpy(np, p, old < n ? old : n);
    return np;
  }
  if (!real_realloc) return NULL;
  return strip(real_realloc(retag(p), n));
}

void* reallocarray(void* p, size_t n, size_t s) {
  ensure_resolved();
  size_t total;
  if (__builtin_mul_overflow(n, s, &total)) return NULL;
  if (real_reallocarray) return strip(real_reallocarray(retag(p), n, s));
  return realloc(p, total);
}

void* memalign(size_t align, size_t n) {
  ensure_resolved();
  if (!real_memalign) return NULL;
  return strip(real_memalign(align, n));
}

void* aligned_alloc(size_t align, size_t n) {
  ensure_resolved();
  if (real_aligned_alloc) return strip(real_aligned_alloc(align, n));
  if (real_memalign) return strip(real_memalign(align, n));
  return NULL;
}

void* valloc(size_t n) {
  ensure_resolved();
  if (real_valloc) return strip(real_valloc(n));
  if (real_memalign) return strip(real_memalign((size_t)sysconf(_SC_PAGESIZE), n));
  return NULL;
}

void* pvalloc(size_t n) {
  ensure_resolved();
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t rounded = (n + page - 1) & ~(page - 1);
  if (real_pvalloc) return strip(real_pvalloc(n));
  return memalign(page, rounded ? rounded : page);
}

int posix_memalign(void** out, size_t align, size_t n) {
  ensure_resolved();
  if (!real_posix_memalign) return 1;
  int rc = real_posix_memalign(out, align, n);
  if (rc == 0 && out) *out = strip(*out);
  return rc;
}

void free(void* p) {
  if (!p) return;
  if (in_arena(p)) return; /* arena chunks are never reclaimed */
  ensure_resolved();
  if (!real_free) return;
  real_free(retag(p));
}

size_t malloc_usable_size(const void* p) {
  if (!p) return 0;
  if (in_arena(p)) return arena_usable((void*)p);
  ensure_resolved();
  if (!real_malloc_usable_size) return 0;
  return real_malloc_usable_size(retag((void*)p));
}
