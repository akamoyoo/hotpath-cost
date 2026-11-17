#define main driver_main
#include "main.cpp"
#undef main
#include "../bench_common.h"
int main() {
  static char buf[1 << 13]; size_t n = 0; long acc = 0; int seq = 0;
  run_ops({{"write_new_order", [&] { n = hp_entry::fix_write_new_order(buf, buf + sizeof buf, ++seq, TP{}, 50001, 100); }},
           {"read_all", [&] { acc += hp_entry::fix_read_all(buf, buf + n); }},
           {"check", [&] { acc += hp_entry::fix_check(buf, buf + n); }}});
  return acc == 42;
}
