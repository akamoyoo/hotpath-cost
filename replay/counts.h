// Per-thread counts of heap operations and mutex acquisitions, for counting what one library operation
// does. Include in exactly one translation unit of a workload executable: the definitions below replace
// glibc's malloc family and pthread_mutex_lock for the whole process (libstdc++'s operator new included).
#pragma once
#include <dlfcn.h>
#include <pthread.h>
#include <cstddef>
#include <cstdint>

extern "C" void *__libc_malloc(size_t);
extern "C" void *__libc_calloc(size_t, size_t);
extern "C" void *__libc_realloc(void *, size_t);
extern "C" void __libc_free(void *);
extern "C" void *__libc_memalign(size_t, size_t);

struct Counts { uint64_t alloc = 0, free = 0, lock = 0; };
static thread_local Counts tl_counts;
// Attribution: while tl_trace is set, each allocation and lock on this thread records its return
// addresses (up to 12 frames) into a fixed table; trace_dump() prints them symbolized, most frequent first.
#include <execinfo.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
struct TraceRec { void *pc[12]; int n; char kind; uint64_t count; };
static TraceRec g_trace[256]; static int g_ntrace = 0;
static thread_local bool tl_trace = false, tl_in_trace = false;
static void trace_hit(char kind) {
  if (!tl_trace || tl_in_trace) return;
  tl_in_trace = true;
  void *pc[14]; int n = backtrace(pc, 14);
  int m = n > 2 ? n - 2 : 0; void **p = pc + 2;   // drop trace_hit and the wrapper
  if (m > 12) m = 12;
  int i = 0;
  for (; i < g_ntrace; i++)
    if (g_trace[i].kind == kind && g_trace[i].n == m && !memcmp(g_trace[i].pc, p, m * sizeof(void *))) break;
  if (i == g_ntrace && g_ntrace < 256) { memcpy(g_trace[i].pc, p, m * sizeof(void *)); g_trace[i].n = m; g_trace[i].kind = kind; g_trace[i].count = 0; g_ntrace++; }
  if (i < g_ntrace) g_trace[i].count++;
  tl_in_trace = false;
}
static void trace_dump(FILE *f, uint64_t calls) {
  tl_in_trace = true;
  for (int i = 0; i < g_ntrace; i++) {
    std::fprintf(f, "%c %.3f per call\n", g_trace[i].kind, double(g_trace[i].count) / calls);
    for (int k = 0; k < g_trace[i].n; k++) {   // object and offset, symbolized later with addr2line
      Dl_info d; void *pc = (char *)g_trace[i].pc[k] - 1;
      if (dladdr(pc, &d) && d.dli_fname) std::fprintf(f, "    %s 0x%lx\n", d.dli_fname, (unsigned long)((char *)pc - (char *)d.dli_fbase));
      else std::fprintf(f, "    ? %p\n", pc);
    }
  }
  g_ntrace = 0; tl_in_trace = false;
}
static inline Counts counts_now() { return tl_counts; }

extern "C" {
void *malloc(size_t n) noexcept { tl_counts.alloc++; trace_hit('a'); return __libc_malloc(n); }
void *calloc(size_t a, size_t b) noexcept { tl_counts.alloc++; trace_hit('a'); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) noexcept { tl_counts.alloc++; trace_hit('a'); return __libc_realloc(p, n); }
void free(void *p) noexcept { if (p) tl_counts.free++; __libc_free(p); }
void *aligned_alloc(size_t a, size_t n) noexcept { tl_counts.alloc++; return __libc_memalign(a, n); }
void *memalign(size_t a, size_t n) noexcept { tl_counts.alloc++; return __libc_memalign(a, n); }
int posix_memalign(void **r, size_t a, size_t n) noexcept {
  tl_counts.alloc++; void *p = __libc_memalign(a, n); if (!p) return 12; *r = p; return 0;
}
static int (*real_mutex_lock)(pthread_mutex_t *) = nullptr;
int pthread_mutex_lock(pthread_mutex_t *m) noexcept {
  tl_counts.lock++; trace_hit('l');
  return real_mutex_lock ? real_mutex_lock(m) : 0;   // null only while resolving, before main and threads
}
}
__attribute__((constructor(101))) static void counts_init() {
  real_mutex_lock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
}
extern "C" {
}
