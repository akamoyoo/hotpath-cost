#define main driver_main
#include "main.cpp"
#undef main
#include "../workload.h"
int main() {
  // Liquibook's SimpleOrderBook with a resting book of 2000 orders on each side (10 price levels per
  // side); the operation adds one order at a price outside the book, which does not trade, and cancels it.
  Book b;
  std::vector<SimpleOrder *> rest;
  for (int i = 0; i < 4000; i++) { bool buy = i & 1; rest.push_back(new SimpleOrder(buy, buy ? 990 - i % 10 : 1010 + i % 10, 10)); b.add(rest.back()); }
  std::vector<SimpleOrder *> pool; for (int i = 0; i < 1000; i++) pool.push_back(new SimpleOrder(i & 1, (i & 1) ? 980 : 1020, 5));
  size_t k = 0;
  run_ops({{"add+cancel", [&] { SimpleOrder *o = pool[k++ % pool.size()]; hp_entry::lb_add(b, o); hp_entry::lb_cancel(b, o); }}}, 1000, 20000);
}
