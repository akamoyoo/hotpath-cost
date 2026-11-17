// ridiculousfish/libdivide: division of 64-bit values by a divisor known only at run time, with
// libdivide's precomputed divider, against the hardware division (the MPMCQueue case).
#include "libdivide.h"
#include <cstdint>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY uint64_t ld_div(const libdivide::divider<uint64_t> &d, const uint64_t *x) { uint64_t s = 0; for (int i = 0; i < 64; i++) s += x[i] / d; return s; }
HP_ENTRY uint64_t hw_div(uint64_t d, const uint64_t *x) { uint64_t s = 0; for (int i = 0; i < 64; i++) s += x[i] / d; return s; }
}
int main() {
  static uint64_t x[64]; for (int i = 0; i < 64; i++) x[i] = 0x9E3779B97F4A7C15ULL * (i + 1) >> 20;
  volatile uint64_t dv = 1000; uint64_t d = dv; libdivide::divider<uint64_t> ld(d); uint64_t acc = 0;
  run_ops({{"64 divisions, libdivide", [&] { acc += hp_entry::ld_div(ld, x); }},
           {"64 divisions, hardware", [&] { acc += hp_entry::hw_div(d, x); }}});
  return acc == 1;
}
