// A virtual call with two implementations, one of which allocates.
#include "../../fx_test.h"
#include <vector>
struct Sink { virtual void on(int x) = 0; virtual ~Sink() = default; };
struct Quiet : Sink { long sum = 0; void on(int x) override { sum += x; } };
struct Loud : Sink { std::vector<int> log; void on(int x) override { log.push_back(x); } };
FX_NOINLINE void dispatch(Sink& s, int x) FX_NB { s.on(x); }
FX_NOINLINE long only_quiet(Quiet& q, int x) FX_NB { q.Quiet::on(x); return q.sum; }
Sink* make(int k) { if (k) return new Loud; return new Quiet; }
int main(int argc, char**) {
  Sink* a = make(0); Sink* b = make(argc);
  Quiet q;
  for (int i = 0; i < 100; i++) { dispatch(*a, i); dispatch(*b, i); only_quiet(q, i); }
  delete a; delete b;
  return 0;
}
