// vector::at and -D_GLIBCXX_ASSERTIONS: which check sites survive -O2.
#include "../../fx_test.h"
#include <vector>
FX_NOINLINE long sum_at(const std::vector<long>& v, const std::vector<int>& idx) FX_NB {
  long s = 0; for (size_t i = 0; i < idx.size(); i++) s += v.at(idx[i]); return s; }
FX_NOINLINE long sum_idx(const std::vector<long>& v, const std::vector<int>& idx) FX_NB {
  long s = 0; for (size_t i = 0; i < idx.size(); i++) s += v[idx[i]]; return s; }
FX_NOINLINE long sum_range(const std::vector<long>& v) FX_NB { long s = 0; for (long x : v) s += x; return s; }
FX_NOINLINE long front_back(const std::vector<long>& v) FX_NB { return v.front() + v.back(); }
int main(int argc, char**) {
  std::vector<long> v(100, argc); std::vector<int> idx(50);
  for (int i = 0; i < 50; i++) idx[i] = i * 2;
  return (int)((sum_at(v, idx) + sum_idx(v, idx) + sum_range(v) + front_back(v)) & 1);
}
