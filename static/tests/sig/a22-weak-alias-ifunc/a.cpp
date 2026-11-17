// Direct calls whose callee is not simply "the function with this body".
#include <cstdlib>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
extern "C" {
NI void impl_loud(long x) { keep = malloc(x + 8); }
NI void impl_quiet(long x) { sink = x; }
// (1) a weak alias of a strong function: the symbol `hook` is replaceable at link time, impl_loud is not
void hook(long x) __attribute__((weak, alias("impl_loud")));
void hookq(long x) __attribute__((weak, alias("impl_quiet")));
// (2) a strong alias (not replaceable)
void strong_alias(long x) __attribute__((alias("impl_loud")));
// (3) an ifunc whose resolver picks at load time
static void (*pick(void))(long) { return getenv("QUIET") ? impl_quiet : impl_loud; }
void viaifunc(long x) __attribute__((ifunc("_ZL4pickv")));
// (4) a weak definition
__attribute__((weak)) NI void weakdef(long x) { keep = malloc(x + 8); }
}
NI void call_weak_alias(long x) { hook(x); }
NI void call_weak_alias_q(long x) { hookq(x); }
NI void call_strong_alias(long x) { strong_alias(x); }
NI void call_ifunc(long x) { viaifunc(x); }
NI void call_weakdef(long x) { weakdef(x); }
// (5) linkonce_odr / inline function: one body in the module
inline NI void inl(long x) { keep = malloc(x + 8); }
NI void call_inline(long x) { inl(x); }
int main() { call_weak_alias(1); call_weak_alias_q(1); call_strong_alias(1); call_ifunc(1); call_weakdef(1); call_inline(1); return 0; }
