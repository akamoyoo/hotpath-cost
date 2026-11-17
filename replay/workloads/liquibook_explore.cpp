// Liquibook: a seeded random order flow, to reach the paths a workload of resting orders does not take.
// Every action (and every order object) is prepared before measurement, so only what the book itself
// allocates is counted. Mix per action: resting limit 35 %, crossing limit 15 %, market 5 %, stop 7 %,
// all-or-none 5 %, immediate-or-cancel 5 %, fill-or-kill 3 %, cancel 15 %, replace 10 %.
// Used with replay/combine.py: which of the sites the static analysis lists does a realistic flow reach?
#define main driver_main
#include "main.cpp"
#undef main
#include "../workload.h"
#include <cstdint>
#include <vector>

namespace lb = liquibook::book;
struct Act { int kind; SimpleOrder *o; lb::OrderConditions c; size_t target; int dq; lb::Price p; };

int main() {
  uint64_t x = 0x2545F4914F6CDD1DULL;
  auto rnd = [&x] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  const long n = 400000;
  std::vector<Act> acts; acts.reserve(n);
  std::vector<SimpleOrder *> orders; orders.reserve(n);
  long added = 0;
  for (long i = 0; i < n; i++) {
    int r = int(rnd() % 100); bool buy = rnd() & 1;
    lb::Price mid = 1000, away = lb::Price(1 + rnd() % 20), cross = lb::Price(rnd() % 5);
    lb::Quantity q = lb::Quantity(1 + rnd() % 50);
    Act a{0, nullptr, 0, 0, 0, 0};
    auto make = [&](lb::Price px, lb::Price stop, lb::OrderConditions c) {
      a.o = new SimpleOrder(buy, px, q, stop, c); a.c = c; orders.push_back(a.o); added++;
    };
    if (r < 35) make(buy ? mid - away : mid + away, 0, 0);
    else if (r < 50) make(buy ? mid + cross : mid - cross, 0, 0);
    else if (r < 55) make(0, 0, 0);   // market order: price 0
    else if (r < 62) make(buy ? mid + away : mid - away, buy ? mid + cross : mid - cross, lb::OrderCondition::oc_stop);
    else if (r < 67) make(buy ? mid + cross : mid - cross, 0, lb::OrderCondition::oc_all_or_none);
    else if (r < 72) make(buy ? mid + cross : mid - cross, 0, lb::OrderCondition::oc_immediate_or_cancel);
    else if (r < 75) make(buy ? mid + cross : mid - cross, 0, lb::OrderCondition::oc_fill_or_kill);
    else if (r < 90 && added > 0) { a.kind = 1; a.target = size_t(rnd() % added); }
    else if (added > 0) { a.kind = 2; a.target = size_t(rnd() % added); a.dq = int(rnd() % 11) - 5; a.p = mid + 10 - lb::Price(rnd() % 21); }
    else make(mid, 0, 0);
    acts.push_back(a);
  }
  Book b; size_t k = 0;
  run_ops({{"random order flow", [&] {
    const Act &a = acts[k++ % acts.size()];
    if (a.kind == 0) hp_entry::lb_add_cond(b, a.o, a.c);
    else if (a.kind == 1) hp_entry::lb_cancel(b, orders[a.target]);
    else hp_entry::lb_replace(b, orders[a.target], a.dq, a.p);
  }}}, 1000, 200000);
}
