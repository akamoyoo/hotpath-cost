// Custom allocators: an arena whose allocate() refills from malloc, and jemalloc's mallocx.
#include <cstddef>
#include <cstdlib>
#define NOINL __attribute__((noinline))
extern "C" void* mallocx(size_t size, int flags);   // jemalloc, linked from a library
struct Arena {
  char* cur = nullptr; char* end = nullptr;
  NOINL void* allocate(size_t n) {
    if (static_cast<size_t>(end - cur) < n) { cur = static_cast<char*>(std::malloc(1 << 20)); end = cur + (1 << 20); }
    void* p = cur; cur += n; return p;
  }
};
NOINL void* arena_hot(Arena& a, size_t n) { return a.allocate(n); }
NOINL void* je_hot(size_t n) { return mallocx(n, 0); }
