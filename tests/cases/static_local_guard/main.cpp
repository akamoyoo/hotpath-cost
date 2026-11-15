// A function-local static with a dynamic initialiser: guard check, __cxa_guard_acquire, allocation.
#include "../../fx_test.h"
#include <vector>
FX_NOINLINE std::vector<int> build() { std::vector<int> v(256); for (int i = 0; i < 256; i++) v[i] = i * i; return v; }
FX_NOINLINE int lookup(int i) FX_NB { static const std::vector<int> t = build(); return t[i & 255]; }
int main(int argc, char**) { int r = 0; for (int i = 0; i < 100; i++) r += lookup(i + argc); return r & 1; }
