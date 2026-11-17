// google/highway (static dispatch: the target -march selects, no runtime dispatch): sum and dot product
// of 1024 floats with explicit vectors.
#include "hwy/highway.h"
#include <cstddef>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hn = hwy::HWY_NAMESPACE;
namespace hp_entry {
HP_ENTRY float hw_sum(const float *a, size_t n) {
  const hn::ScalableTag<float> d; auto acc = hn::Zero(d); size_t i = 0;
  for (; i + hn::Lanes(d) <= n; i += hn::Lanes(d)) acc = hn::Add(acc, hn::LoadU(d, a + i));
  float s = hn::ReduceSum(d, acc); for (; i < n; i++) s += a[i]; return s;
}
HP_ENTRY float hw_dot(const float *a, const float *b, size_t n) {
  const hn::ScalableTag<float> d; auto acc = hn::Zero(d); size_t i = 0;
  for (; i + hn::Lanes(d) <= n; i += hn::Lanes(d)) acc = hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), acc);
  float s = hn::ReduceSum(d, acc); for (; i < n; i++) s += a[i] * b[i]; return s;
}
}
int main() {
  static float a[1024], b[1024]; for (int i = 0; i < 1024; i++) { a[i] = 0.001f * i; b[i] = 1.0f - 0.0005f * i; }
  float acc = 0;
  run_ops({{"sum of 1024 floats", [&] { acc += hp_entry::hw_sum(a, 1024); }},
           {"dot of 1024 floats", [&] { acc += hp_entry::hw_dot(a, b, 1024); }}});
  return acc == 1;
}
