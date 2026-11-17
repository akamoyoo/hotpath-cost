// cameron314/readerwriterqueue: moodycamel::ReaderWriterQueue (SPSC), one thread, enqueue then dequeue.
#include "readerwriterqueue.h"
#include <cstdint>
#include "../drivers/hp_entry.h"
#include "../workload.h"
struct Msg { uint64_t seq; double px; int qty; };
using Q = moodycamel::ReaderWriterQueue<Msg>;
namespace hp_entry {
HP_ENTRY bool rw_try_enqueue(Q &q, const Msg &m) { return q.try_enqueue(m); }
HP_ENTRY bool rw_enqueue(Q &q, const Msg &m) { return q.enqueue(m); }
HP_ENTRY bool rw_try_dequeue(Q &q, Msg &m) { return q.try_dequeue(m); }
}
int main() {
  Q q(1024); Msg m{1, 2.0, 3};
  run_ops({{"try_enqueue+try_dequeue", [&] { hp_entry::rw_try_enqueue(q, m); hp_entry::rw_try_dequeue(q, m); }},
           {"enqueue+try_dequeue", [&] { hp_entry::rw_enqueue(q, m); hp_entry::rw_try_dequeue(q, m); }}});
}
