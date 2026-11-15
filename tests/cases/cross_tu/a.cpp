// The entry is here; the hazard is in another translation unit (b.cpp).
#include "../../fx_test.h"
int helper(int);
FX_NOINLINE int process(int x) FX_NB { return helper(x) + 1; }
int main(int argc, char**) { int r = 0; for (int i = 0; i < 10; i++) r += process(i + argc); return r & 1; }
