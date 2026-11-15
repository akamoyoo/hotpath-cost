// Each entry hides an allocation (or, for the signal-safety style checks, a library call) behind one
// construct. The tool must never answer none for it.
#include "shapes.hpp"
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdexcept>
#include <vector>
#define NOINL __attribute__((noinline))
// 1. virtual call: the only allocating override is in another translation unit
NOINL long g_virtual(const Shape& s) { return s.area(); }
// 2a. function pointer cast to an unrelated type and back (legal)
int* volatile g_keep_main;
static int alloc_int(int x) { g_keep_main = (int*)std::malloc(4); return x; }
using Erased = void (*)();
Erased g_erased = reinterpret_cast<Erased>(&alloc_int);
NOINL int g_cast_back(int x) { return reinterpret_cast<int (*)(int)>(g_erased)(x); }
// 2b. called through the wrong type (undefined behaviour): int(int) called as int(long)
static int other_long(long x) { return (int)x; }
int (*volatile g_wrong)(long) = &other_long;
NOINL int g_cast_wrong(long x) { return reinterpret_cast<int (*)(long)>(g_erased)(x); }
// 3. destructor and catch handler that run only while unwinding
std::vector<int>* volatile g_log;
struct Guard { bool armed = true; ~Guard() { if (armed) g_log = new std::vector<int>(4); } };
NOINL void may_throw(int x) { if (x == 42) throw std::runtime_error("x"); }
NOINL int g_unwind_dtor(int x) { Guard g; may_throw(x); g.armed = false; return x; }
NOINL int g_unwind_catch(int x) { try { may_throw(x); } catch (...) { g_log = new std::vector<int>(2); } return x; }
// 4. a hook installed by a global constructor in another translation unit
NOINL int g_global_ctor(int x) { return g_hook ? g_hook(x) : x; }
// 5. intrinsics lowered to library calls (checked with the test-libc policy)
struct Big { long v[64]; };
Big g_big_a, g_big_b;
NOINL void g_struct_copy(int) { g_big_a = g_big_b; }                         // llvm.memcpy -> memcpy
NOINL double g_frem(double a, double b) { return __builtin_fmod(a, b); }     // frem -> fmod
NOINL double g_floor(double a) { return __builtin_floor(a); }                // llvm.floor -> floor (no SSE4.1)
// 6. tail call
NOINL int leaf_alloc(int x) { g_log = new std::vector<int>(x & 7); return x; }
NOINL int g_tail(int x) { return leaf_alloc(x); }
// 7. lambdas stored in a struct
NOINL int g_lambda_fp(Holder& h, int x) { return h.fp(x); }
NOINL int g_lambda_fn(Holder& h, int x) { return h.cb(x); }
// 8. exit() runs atexit handlers
static void at_exit_alloc() { g_log = new std::vector<int>(1); }
NOINL void g_exit(int x) { if (x == 99) std::exit(1); }
// 9. dynamic loading and executable mappings
NOINL int g_dlsym(const char* name, int x) {
  void* h = dlopen("libm.so.6", RTLD_NOW);
  auto f = reinterpret_cast<int (*)(int)>(dlsym(h, name));
  void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  (void)code;
  return f ? f(x) : x;
}
// 10. inline assembly that calls
extern "C" void hidden_alloc() { g_log = new std::vector<int>(3); }
NOINL void g_asm_call() { asm volatile("call hidden_alloc" ::: "memory", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"); }
// 11. a virtual call on a standard-library type (derived classes may live in libstdc++)
NOINL size_t g_std_virtual(const std::exception& e) { return std::strlen(e.what()); }
int main(int argc, char**) {
  std::atexit(at_exit_alloc);
  Holder h; fill_holder(h);
  Shape* s = make_shape(argc);
  long r = g_virtual(*s) + g_cast_back(argc) + g_unwind_dtor(argc) + g_unwind_catch(argc) + g_global_ctor(argc) + g_tail(argc)
         + g_lambda_fp(h, argc) + g_lambda_fn(h, argc) + (argc > 5 ? g_cast_wrong(argc) + g_wrong(argc) : 0);
  g_struct_copy(argc); r += (long)g_frem(argc, 3) + (long)g_floor(argc);
  g_exit(argc);
  if (argc > 7) { r += g_dlsym("abs", argc); g_asm_call(); }
  std::runtime_error e("x"); r += (long)g_std_virtual(e);
  return (int)(r & 1);
}
