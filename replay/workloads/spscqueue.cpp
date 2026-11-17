#define main driver_main
#include "main.cpp"
#undef main
#include "../workload.h"
int main() {
  Q q(1024); Msg m{1, 2.0, 3}, o{};
  run_ops({{"try_push+consume", [&] { hp_entry::spsc_try_push(q, m); hp_entry::spsc_consume(q, o); }},
           {"try_emplace+front+pop", [&] { hp_entry::spsc_try_emplace(q, 1, 2.0, 3); hp_entry::spsc_front(q); hp_entry::spsc_pop(q); }}});
}
