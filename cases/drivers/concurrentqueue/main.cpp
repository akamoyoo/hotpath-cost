// cameron314/concurrentqueue v1.0.4. README: "try_enqueue, unlike just plain enqueue, will never
// allocate memory"; try_dequeue "never allocates". enqueue is documented to allocate if needed.
#include "concurrentqueue.h"
#include <cstdint>
#include "../fx_entry.h"

struct Msg { uint64_t seq; double px; int qty; };
using Q = moodycamel::ConcurrentQueue<Msg>;

namespace fx_entry {
FX_ENTRY bool cq_try_enqueue(Q &q, const Msg &m) { return q.try_enqueue(m); }                // implicit producer
FX_ENTRY bool cq_try_enqueue_tok(Q &q, moodycamel::ProducerToken &t, const Msg &m) { return q.try_enqueue(t, m); }
FX_ENTRY bool cq_try_dequeue(Q &q, Msg &m) { return q.try_dequeue(m); }
FX_ENTRY bool cq_try_dequeue_tok(Q &q, moodycamel::ConsumerToken &t, Msg &m) { return q.try_dequeue(t, m); }
FX_ENTRY bool cq_enqueue(Q &q, const Msg &m) { return q.enqueue(m); }                        // documented to allocate
}

int main() {
  Q q(1024);
  moodycamel::ProducerToken pt(q);
  moodycamel::ConsumerToken ct(q);
  Msg m{1, 2.0, 3};
  fx_entry::cq_try_enqueue(q, m); fx_entry::cq_try_enqueue_tok(q, pt, m); fx_entry::cq_try_dequeue(q, m);
  fx_entry::cq_try_dequeue_tok(q, ct, m); return fx_entry::cq_enqueue(q, m);
}
