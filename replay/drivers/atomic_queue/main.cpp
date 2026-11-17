// max0x7ba/atomic_queue v1.9.4: fixed-size (AtomicQueue, AtomicQueue2) and run-time-size (AtomicQueueB2) queues.
#include <atomic_queue/atomic_queue.h>
#include <cstdint>
#include "../hp_entry.h"

struct Msg { uint64_t seq; double px; int qty; };
using QA = atomic_queue::AtomicQueue<uint32_t, 1024>;           // atomic elements
using QA2 = atomic_queue::AtomicQueue2<Msg, 1024>;              // non-atomic elements
using QB2 = atomic_queue::AtomicQueueB2<Msg>;                   // heap ring buffer, size at run time
using QB = atomic_queue::AtomicQueueB<uint32_t>;                // heap ring buffer of atomics

namespace hp_entry {
HP_ENTRY bool aq_try_push(QA &q, uint32_t v) { return q.try_push(v); }
HP_ENTRY bool aq_try_pop(QA &q, uint32_t &v) { return q.try_pop(v); }
HP_ENTRY void aq_push(QA &q, uint32_t v) { q.push(v); }
HP_ENTRY uint32_t aq_pop(QA &q) { return q.pop(); }
HP_ENTRY bool aq2_try_push(QA2 &q, const Msg &m) { return q.try_push(m); }
HP_ENTRY bool aq2_try_pop(QA2 &q, Msg &m) { return q.try_pop(m); }
HP_ENTRY bool aqb2_try_push(QB2 &q, const Msg &m) { return q.try_push(m); }
HP_ENTRY bool aqb2_try_pop(QB2 &q, Msg &m) { return q.try_pop(m); }
HP_ENTRY bool aqb_try_push(QB &q, uint32_t v) { return q.try_push(v); }
HP_ENTRY bool aqb_try_pop(QB &q, uint32_t &v) { return q.try_pop(v); }
}

int main() {
  static QA qa; static QA2 qa2; QB qb(1024); QB2 qb2(1024);
  uint32_t v = 1; Msg m{1, 2.0, 3};
  hp_entry::aq_try_push(qa, v); hp_entry::aq_try_pop(qa, v); hp_entry::aq_push(qa, v); hp_entry::aq_pop(qa);
  hp_entry::aq2_try_push(qa2, m); hp_entry::aq2_try_pop(qa2, m); hp_entry::aqb2_try_push(qb2, m);
  hp_entry::aqb2_try_pop(qb2, m); hp_entry::aqb_try_push(qb, v); return hp_entry::aqb_try_pop(qb, v);
}
