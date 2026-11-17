// concurrentqueue: a seeded random sequence of bursts on one thread, to reach the paths a steady
// enqueue/dequeue pair does not take: enqueue bursts of 1 to 3000 elements (beyond the initial capacity,
// so blocks and block indexes are allocated) with and without a producer token, try_enqueue bursts
// that may fail when the queue is full, and as many dequeue bursts, so the queue's length wanders up and
// down instead of growing. Every action is prepared before measurement.
#define main driver_main
#include "main.cpp"
#undef main
#include "../workload.h"
#include <cstdint>
#include <vector>
struct Act { int kind; int n; };
int main() {
  uint64_t x = 0x9E3779B97F4A7C15ULL; auto rnd = [&x] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  std::vector<Act> acts; acts.reserve(200000);
  for (int i = 0; i < 200000; i++) {
    int r = int(rnd() % 100), n = 1 + int(rnd() % (r % 10 == 0 ? 3000 : 64));
    acts.push_back({r < 20 ? 0 : r < 30 ? 1 : r < 40 ? 2 : r < 70 ? 3 : 4, n});   // as many dequeues as enqueues
  }
  Q q(64); moodycamel::ProducerToken pt(q); moodycamel::ConsumerToken ct(q); Msg m{1, 2.0, 3};
  size_t k = 0;
  run_ops({{"random bursts", [&] {
    const Act &a = acts[k++ % acts.size()];
    for (int i = 0; i < a.n; i++) {
      switch (a.kind) {
        case 0: hp_entry::cq_enqueue(q, m); break;                 // may allocate a block
        case 1: hp_entry::cq_try_enqueue(q, m); break;             // fails when full
        case 2: hp_entry::cq_try_enqueue_tok(q, pt, m); break;
        case 3: hp_entry::cq_try_dequeue_tok(q, ct, m); break;
        default: hp_entry::cq_try_dequeue(q, m); break;
      }
    }
  }}}, 100, 2000);
}
