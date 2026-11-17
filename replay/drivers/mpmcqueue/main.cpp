// rigtorp/MPMCQueue v1.0: the documented push/pop operations.
#include <rigtorp/MPMCQueue.h>
#include <cstdint>
#include "../hp_entry.h"

struct Msg { uint64_t seq; double px; int qty; };
using Q = rigtorp::MPMCQueue<Msg>;

namespace hp_entry {
HP_ENTRY void mpmc_push(Q &q, const Msg &m) { q.push(m); }
HP_ENTRY bool mpmc_try_push(Q &q, const Msg &m) { return q.try_push(m); }
HP_ENTRY void mpmc_pop(Q &q, Msg &m) { q.pop(m); }
HP_ENTRY bool mpmc_try_pop(Q &q, Msg &m) { return q.try_pop(m); }
}

int main() {
  Q q(1024);
  Msg m{1, 2.0, 3};
  hp_entry::mpmc_push(q, m); hp_entry::mpmc_try_push(q, m); hp_entry::mpmc_pop(q, m);
  return hp_entry::mpmc_try_pop(q, m);
}
