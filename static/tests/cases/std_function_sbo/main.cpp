// std::function: a capture that fits the small buffer (16 bytes in libstdc++) and one that does not.
#include "../../fx_test.h"
#include <functional>
#include <cstring>
struct Big { char b[64]; };
FX_NOINLINE int build_small(int a) FX_NB { std::function<int(int)> f = [a](int x) { return x + a; }; return f(1); }
FX_NOINLINE int build_big(int a) FX_NB { Big big; std::memset(big.b, a, sizeof big.b);
  std::function<int(int)> f = [big](int x) { return x + big.b[3]; }; return f(1); }
FX_NOINLINE int call_fn(const std::function<int(int)>& f, int x) FX_NB { return f(x); }
int main(int argc, char**) {
  std::function<int(int)> g = [argc](int x) { return x * argc; };
  int r = 0;
  for (int i = 0; i < 10; i++) r += build_small(i) + build_big(i) + call_fn(g, i);
  return r & 1;
}
