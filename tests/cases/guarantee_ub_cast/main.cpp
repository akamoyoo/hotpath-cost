// The one construct where `none` is wrong by design: a call through a function pointer of the wrong type.
// This is undefined behaviour in C++ (assumption 1), and LLVM IR has no record of the original type.
#include <cstdlib>
#define NOINL __attribute__((noinline))
int* volatile g_keep;
static int alloc_int(int x) { g_keep = (int*)std::malloc(4); return x; }
static int other_long(long x) { return (int)x; }
using Erased = void (*)();
Erased g_erased = reinterpret_cast<Erased>(&alloc_int);
int (*volatile g_other)(long) = &other_long;
NOINL int g_cast_wrong(long x) { return reinterpret_cast<int (*)(long)>(g_erased)(x); }
int main(int argc, char**) { return g_cast_wrong(argc) + g_other(argc); }
