// A direct call to an ifunc symbol whose resolver picks an allocating implementation by CPU feature;
// a call through an alias; a weak definition overridden by a strong one in another translation unit.
#include <cstdio>
#include <cstdlib>
#define LOUD() do { void* p_ = std::malloc(48); asm volatile("" : : "r"(p_) : "memory"); std::free(p_); } while (0)
extern "C" long impl_plain(long x) { return x + 1; }
extern "C" long impl_fast(long x) { LOUD(); return x + 2; }
extern "C" void* pick() { __builtin_cpu_init(); return __builtin_cpu_supports("sse2") ? (void*)impl_fast : (void*)impl_plain; }
extern "C" long ifn(long) __attribute__((ifunc("pick")));
__attribute__((noinline)) long entry_ifunc(long x) { return ifn(x) + 1; }
extern "C" long real_loud(long x) { LOUD(); return x; }
extern "C" long aliased(long) __attribute__((alias("real_loud")));
__attribute__((noinline)) long entry_alias(long x) { return aliased(x) + 1; }
extern "C" __attribute__((weak, noinline)) long hookw(long x) { return x; }
__attribute__((noinline)) long entry_weak(long x) { return hookw(x) + 1; }
int main(int argc, char**) { std::printf("%ld\n", entry_ifunc(argc) + entry_alias(argc) + entry_weak(argc)); }
