#define main driver_main
#include "main.cpp"
#undef main
#include "../workload.h"
int main() {
  Q q(1024); Msg m{1, 2.0, 3};
  run_ops({{"push+pop", [&] { hp_entry::mpmc_push(q, m); hp_entry::mpmc_pop(q, m); }},
           {"try_push+try_pop", [&] { hp_entry::mpmc_try_push(q, m); hp_entry::mpmc_try_pop(q, m); }}});
}
