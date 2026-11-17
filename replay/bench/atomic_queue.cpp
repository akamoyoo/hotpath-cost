#define main driver_main
#include "main.cpp"
#undef main
#include "../bench_common.h"
int main() {
  static QA qa; static QA2 qa2; QB qb(1024); QB2 qb2(1024); uint32_t v = 1; Msg m{1, 2.0, 3};
  run_ops({{"AtomicQueue push+pop", [&] { hp_entry::aq_push(qa, v); v = hp_entry::aq_pop(qa); }},
           {"AtomicQueue2 try_push+try_pop", [&] { hp_entry::aq2_try_push(qa2, m); hp_entry::aq2_try_pop(qa2, m); }},
           {"AtomicQueueB2 try_push+try_pop", [&] { hp_entry::aqb2_try_push(qb2, m); hp_entry::aqb2_try_pop(qb2, m); }}});
}
