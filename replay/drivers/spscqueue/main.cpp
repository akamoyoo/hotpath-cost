// rigtorp/SPSCQueue v1.1: the documented producer and consumer operations.
#include <rigtorp/SPSCQueue.h>
#include <cstdint>
#include "../hp_entry.h"

struct Msg { uint64_t seq; double px; int qty; };
using Q = rigtorp::SPSCQueue<Msg>;

namespace hp_entry {
HP_ENTRY bool spsc_try_push(Q &q, const Msg &m) { return q.try_push(m); }
HP_ENTRY void spsc_push(Q &q, const Msg &m) { q.push(m); }
HP_ENTRY bool spsc_try_emplace(Q &q, uint64_t s, double p, int n) { return q.try_emplace(Msg{s, p, n}); }
HP_ENTRY Msg *spsc_front(Q &q) { return q.front(); }
HP_ENTRY void spsc_pop(Q &q) { q.pop(); }
HP_ENTRY bool spsc_consume(Q &q, Msg &out) {   // front + pop, the documented consumer loop body
  Msg *p = q.front();
  if (!p) return false;
  out = *p;
  q.pop();
  return true;
}
}

int main(int argc, char **) {
  Q q(1024);
  Msg m{1, 2.0, 3}, o{};
  hp_entry::spsc_try_push(q, m); hp_entry::spsc_push(q, m); hp_entry::spsc_try_emplace(q, 1, 2.0, argc);
  hp_entry::spsc_front(q); hp_entry::spsc_pop(q); return hp_entry::spsc_consume(q, o);
}
