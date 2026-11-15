// Cases whose only correct precise answer needs the points-to analysis: the type rule says "alloc" for
// every entry here.
#include "../../fx_test.h"
#include <functional>
#include <vector>
std::vector<int>* volatile g_log;

// 1. Two std::function objects of the same signature; only the second allocates.
std::function<int(int)> g_fast = [](int x) { return x + 1; };
std::function<int(int)> g_slow = [](int x) { g_log = new std::vector<int>(x & 7); return x; };
FX_NOINLINE int fast_entry(int x) FX_NB { return g_fast(x); }
FX_NOINLINE int slow_entry(int x) FX_NB { return g_slow(x); }

// 2. A callback table filled at start-up. Another allocating function of the same type exists but is
//    never put into the table.
using Handler = int (*)(int);
FX_NOINLINE int on_quote(int x) { return x * 2; }
FX_NOINLINE int on_trade(int x) { return x * 3; }
FX_NOINLINE int unrelated_alloc(int x) { g_log = new std::vector<int>(x & 3); return x; }
Handler g_table[4];
Handler volatile g_other;
FX_NOINLINE void init_table() { g_table[0] = on_quote; g_table[1] = on_trade; g_other = unrelated_alloc; }
FX_NOINLINE int dispatch(unsigned t, int x) FX_NB { return g_table[t & 1](x); }

// 3. A strategy object chosen by a constructor argument.
struct Strategy { virtual long apply(long) = 0; virtual ~Strategy() = default; };
struct Cheap : Strategy { long apply(long x) override { return x + 1; } };
struct Costly : Strategy { long apply(long x) override { g_log = new std::vector<int>(x & 7); return x; } };
struct Engine {
  Strategy* s;
  __attribute__((always_inline)) explicit Engine(Strategy* st) : s(st) {}
  __attribute__((always_inline)) long run(long x) { return s->apply(x); }
};
FX_NOINLINE Engine* make_fast() { return new Engine(new Cheap); }
FX_NOINLINE Engine* make_slow() { return new Engine(new Costly); }
FX_NOINLINE long run_fast(Engine* e, long x) FX_NB { return e->run(x); }
FX_NOINLINE long run_slow(Engine* e, long x) FX_NB { return e->run(x); }

int main(int argc, char**) {
  init_table();
  Engine* f = make_fast();
  Engine* s = make_slow();
  long r = fast_entry(argc) + slow_entry(argc) + dispatch(argc, argc) + g_other(argc) + run_fast(f, argc) + run_slow(s, argc);
  return (int)(r & 1);
}
