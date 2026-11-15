// Each entry calls through a pointer that reaches an allocating function by a path a careless pointer
// analysis would miss. The only correct answers are reachable or unknown, never none.
#include "../../fx_test.h"
#include <cstring>
#include <pthread.h>
#include <vector>
std::vector<int>* volatile g_log;
using Fn = int (*)(int);
FX_NOINLINE int harmless(int x) { return x + 1; }
#define ALLOC_FN(name, n) FX_NOINLINE int name(int x) { g_log = new std::vector<int>(n); return x; }
ALLOC_FN(alloc_alias, 1)
ALLOC_FN(alloc_memcpy, 2)
ALLOC_FN(alloc_libc_memcpy, 3)
ALLOC_FN(alloc_union, 4)
ALLOC_FN(alloc_roundtrip, 5)
ALLOC_FN(alloc_ctor, 6)

// 1. stored through an alias: a pointer to the field, obtained in another function
struct Holder { long tag; Fn fp; };
Holder g_holder = {0, harmless};
FX_NOINLINE void install(Fn* slot, Fn f) { *slot = f; }
FX_NOINLINE int via_alias(int x) FX_NB { return g_holder.fp(x); }

// 2. copied with memcpy as part of a struct (the intrinsic, and libc memcpy through a pointer)
struct Ops { Fn f; long pad[6]; };
Ops g_src = {alloc_memcpy, {0}};
Ops g_dst = {harmless, {0}};
Ops g_src2 = {alloc_libc_memcpy, {0}};
Ops g_dst2 = {harmless, {0}};
void* (*volatile g_memcpy)(void*, const void*, size_t) = &std::memcpy;
FX_NOINLINE void copy_ops(size_t n) { std::memcpy(&g_dst, &g_src, n); g_memcpy(&g_dst2, &g_src2, n); }
FX_NOINLINE int via_memcpy(int x) FX_NB { return g_dst.f(x); }
FX_NOINLINE int via_libc_memcpy(int x) FX_NB { return g_dst2.f(x); }

// 3. through a union: written as an integer, read as a function pointer
union U { Fn f; long bits; };
U g_u = {harmless};
FX_NOINLINE void set_bits(long v) { g_u.bits = v; }
FX_NOINLINE int via_union(int x) FX_NB { return g_u.f(x); }

// 4. escapes to a library and comes back
Ops g_ops_tls = {alloc_roundtrip, {0}};
pthread_key_t g_key;
FX_NOINLINE int via_library(int x) FX_NB { Ops* o = (Ops*)pthread_getspecific(g_key); return o->f(x); }

// 5. written by a global constructor in another translation unit (ctor.cpp)
extern Fn g_cb;
FX_NOINLINE int via_global_ctor(int x) FX_NB { return g_cb ? g_cb(x) : x; }

int main(int argc, char**) {
  install(&g_holder.fp, alloc_alias);
  copy_ops(sizeof(Ops));
  set_bits((long)(void*)&alloc_union);
  pthread_key_create(&g_key, nullptr);
  pthread_setspecific(g_key, &g_ops_tls);
  return via_alias(argc) + via_memcpy(argc) + via_libc_memcpy(argc) + via_union(argc) + via_library(argc) + via_global_ctor(argc);
}
