// Calls through function pointers stored in a table: a constant table and a mutable one.
#include "../../fx_test.h"
#include <cstdlib>
FX_NOINLINE int op_add(int x) { return x + 1; }
FX_NOINLINE int op_mul(int x) { return x * 3; }
int* volatile g_keep;
FX_NOINLINE int op_alloc(int x) { int* p = (int*)std::malloc(sizeof(int) * (x & 15) + 4); g_keep = p; return x; }
FX_NOINLINE int op_other(int x) { return x - 5; }
static int (*const k_ops[])(int) = {op_add, op_mul, op_alloc};
int (*g_ops[])(int) = {op_add, op_mul, op_other};
FX_NOINLINE int apply_const(unsigned k, int x) FX_NB { return k_ops[k % 3](x); }
FX_NOINLINE int apply_mut(unsigned k, int x) FX_NB { return g_ops[k % 3](x); }
int main(int argc, char**) { int r = 0; for (unsigned i = 0; i < 9; i++) r += apply_const(i, argc) + apply_mut(i, argc); return r & 1; }
