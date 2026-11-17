// orlp/pdqsort: sorting 256 pseudo-random 64-bit integers, and 64 doubles, from the same unsorted copy.
#include "pdqsort.h"
#include <cstdint>
#include <cstring>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
namespace hp_entry {
HP_ENTRY void pdq_u64(uint64_t *a, size_t n) { pdqsort(a, a + n); }
HP_ENTRY void pdq_f64(double *a, size_t n) { pdqsort(a, a + n); }
}
int main() {
  static uint64_t src[256], a[256]; static double fs[64], f[64]; uint64_t x = 88172645463325252ULL;
  for (auto &v : src) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v = x; }
  for (auto &v : fs) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v = double(x % 100000) / 7.0; }
  run_ops({{"256 uint64", [&] { std::memcpy(a, src, sizeof a); hp_entry::pdq_u64(a, 256); }},
           {"64 double", [&] { std::memcpy(f, fs, sizeof f); hp_entry::pdq_f64(f, 64); }}}, 1000, 50000);
  return a[0] == 1;
}
