#define main driver_main
#include "main.cpp"
#undef main
#include "../bench_common.h"
int main() {
  Q q(1024); moodycamel::ProducerToken pt(q); moodycamel::ConsumerToken ct(q); Msg m{1, 2.0, 3};
  run_ops({{"try_enqueue+try_dequeue", [&] { hp_entry::cq_try_enqueue(q, m); hp_entry::cq_try_dequeue(q, m); }},
           {"try_enqueue_tok+try_dequeue_tok", [&] { hp_entry::cq_try_enqueue_tok(q, pt, m); hp_entry::cq_try_dequeue_tok(q, ct, m); }},
           {"enqueue+try_dequeue", [&] { hp_entry::cq_enqueue(q, m); hp_entry::cq_try_dequeue(q, m); }}});
}
