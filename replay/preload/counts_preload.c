// Process-wide counts of heap operations and mutex acquisitions, for an unmodified program: build as a
// shared library and load it with LD_PRELOAD. At exit it writes "alloc N free N lock N" to the file named
// by COUNTS_OUT (stderr if unset). The rules are those of counts.h: realloc counts as an allocation,
// free(NULL) is not counted, the aligned allocators count as allocations, every pthread_mutex_lock counts.
// Unlike counts.h it counts every thread of the process.
//
//   cc -O2 -shared -fPIC -o counts_preload.so counts_preload.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

static atomic_ulong n_alloc, n_free, n_lock;
#define COUNT(x) atomic_fetch_add_explicit(&(x), 1, memory_order_relaxed)

void *malloc(size_t n) { COUNT(n_alloc); return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { COUNT(n_alloc); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { COUNT(n_alloc); return __libc_realloc(p, n); }
void free(void *p) { if (p) COUNT(n_free); __libc_free(p); }
void *aligned_alloc(size_t a, size_t n) { COUNT(n_alloc); return __libc_memalign(a, n); }
void *memalign(size_t a, size_t n) { COUNT(n_alloc); return __libc_memalign(a, n); }
int posix_memalign(void **r, size_t a, size_t n) {
  COUNT(n_alloc);
  void *p = __libc_memalign(a, n);
  if (!p) return 12;
  *r = p;
  return 0;
}

static int (*real_mutex_lock)(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *m) {
  COUNT(n_lock);
  return real_mutex_lock ? real_mutex_lock(m) : 0;   // null only while resolving, before main and threads
}

__attribute__((constructor(101))) static void counts_init(void) {
  real_mutex_lock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
}

__attribute__((destructor)) static void counts_report(void) {
  const char *path = getenv("COUNTS_OUT");
  FILE *f = path ? fopen(path, "w") : stderr;
  if (!f) return;
  fprintf(f, "alloc %lu free %lu lock %lu\n", (unsigned long)atomic_load(&n_alloc),
          (unsigned long)atomic_load(&n_free), (unsigned long)atomic_load(&n_lock));
  if (f != stderr) fclose(f);
}
