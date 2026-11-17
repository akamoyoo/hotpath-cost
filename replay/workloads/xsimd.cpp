// xtensor-stack/xsimd: an explicit-SIMD sum and dot product of 1024 floats (AVX2 batches).
#include <xsimd/xsimd.hpp>
#include <cstddef>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY float xs_sum(const float *a, size_t n) {
  using B = xsimd::batch<float>; B acc(0.f); size_t i = 0;
  for (; i + B::size <= n; i += B::size) acc += B::load_unaligned(a + i);
  float s = xsimd::reduce_add(acc); for (; i < n; i++) s += a[i]; return s;
}
HP_ENTRY float xs_dot(const float *a, const float *b, size_t n) {
  using B = xsimd::batch<float>; B acc(0.f); size_t i = 0;
  for (; i + B::size <= n; i += B::size) acc = xsimd::fma(B::load_unaligned(a + i), B::load_unaligned(b + i), acc);
  float s = xsimd::reduce_add(acc); for (; i < n; i++) s += a[i] * b[i]; return s;
}
}
int main() {
  static float a[1024], b[1024]; for (int i = 0; i < 1024; i++) { a[i] = 0.001f * i; b[i] = 1.0f - 0.0005f * i; }
  float acc = 0;
  run_ops({{"sum of 1024 floats", [&] { acc += hp_entry::xs_sum(a, 1024); }},
           {"dot of 1024 floats", [&] { acc += hp_entry::xs_dot(a, b, 1024); }}});
  return acc == 1;
}
