// Hazards only on error paths: must be tagged cold. One unlikely branch, one plain branch.
#include "../../fx_test.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
static std::vector<int> g_v;
FX_NOINLINE int parse(const char* p, int n) FX_NB {
  if (n < 0) { std::fprintf(stderr, "bad length %d\n", n); throw std::runtime_error("negative length"); }
  int s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  return s;
}
FX_NOINLINE void grow_slow(int n) { g_v.resize(g_v.size() + n); }
FX_NOINLINE int maybe_grow(int n) FX_NB { if (n > 1000) [[unlikely]] grow_slow(n); return (int)g_v.size(); }
FX_NOINLINE int plain_grow(int n) FX_NB { if (n > 1000) grow_slow(n); return (int)g_v.size(); }
int main(int argc, char**) {
  int r = parse("abc", 3) + maybe_grow(argc) + plain_grow(argc);
  try { r += parse("x", -argc); } catch (const std::exception&) { r++; }
  return r & 1;
}
