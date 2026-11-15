// Regression (round 3): a code pointer copied byte by byte (legal for trivially copyable objects) must be
// found. Volatile bytes keep the loop from becoming a memcpy.
#include "../../fx_test.h"
#include <vector>
std::vector<int>* volatile g_log;
using Fn = int (*)(int);
FX_NOINLINE int harmless(int x) { return x + 1; }
FX_NOINLINE int alloc_bytes(int x) { g_log = new std::vector<int>(3); return x; }
struct Ops { Fn f; long tag; };
Ops g_src = {alloc_bytes, 1};
Ops g_dst = {harmless, 2};
FX_NOINLINE void copy_bytes() {
  volatile unsigned char* d = reinterpret_cast<volatile unsigned char*>(&g_dst);
  const volatile unsigned char* s = reinterpret_cast<const volatile unsigned char*>(&g_src);
  for (unsigned i = 0; i < sizeof(Ops); i++) d[i] = s[i];
}
FX_NOINLINE int via_bytes(int x) FX_NB { return g_dst.f(x); }
// a double stored next to a code pointer must not spread the pointer through arithmetic on numbers
struct Mixed { Fn f; double v; };
Mixed g_mixed = {harmless, 1.5};
FX_NOINLINE double scale(double k) { return g_mixed.v * k; }
FX_NOINLINE int via_mixed(int x) FX_NB { return g_mixed.f(x) + (int)scale(2.0); }
int main(int argc, char**) { copy_bytes(); return via_bytes(argc) + via_mixed(argc); }
