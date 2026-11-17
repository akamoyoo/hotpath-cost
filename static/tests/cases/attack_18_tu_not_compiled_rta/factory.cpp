// Compiles with g++ but not with Clang (__builtin_apply_args is GCC-only). fn-effects skips it and marks
// its functions tu-not-compiled. It is the only place that constructs a Loud.
#include "shapes.hpp"
static void* args_of() { return __builtin_apply_args(); }
Base* make_loud(long n) { (void)args_of; return n >= 0 ? new Loud : nullptr; }
