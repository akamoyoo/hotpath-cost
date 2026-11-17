// libeigen/eigen: fixed-size dense kernels (no allocation expected), the part of Eigen most exposed to
// the compiler's vectorizer.
#include <Eigen/Dense>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
using M4 = Eigen::Matrix4d; using M8 = Eigen::Matrix<double, 8, 8>; using V8 = Eigen::Matrix<double, 8, 1>;
namespace hp_entry {
HP_ENTRY void eg_mul4(const M4 &a, const M4 &b, M4 &c) { c.noalias() = a * b; }
HP_ENTRY void eg_mv8(const M8 &a, const V8 &x, V8 &y) { y.noalias() = a * x; }
HP_ENTRY double eg_dot(const Eigen::VectorXd &a, const Eigen::VectorXd &b) { return a.dot(b); }
HP_ENTRY void eg_solve4(const M4 &a, const Eigen::Vector4d &b, Eigen::Vector4d &x) { x = a.llt().solve(b); }
}
int main() {
  M4 a = M4::Random(), b = M4::Random(), c; M8 A = M8::Random(); V8 x = V8::Random(), y;
  M4 spd = a * a.transpose() + 4 * M4::Identity(); Eigen::Vector4d r = Eigen::Vector4d::Random(), s;
  Eigen::VectorXd u = Eigen::VectorXd::Random(256), v = Eigen::VectorXd::Random(256); double acc = 0;
  run_ops({{"Matrix4d * Matrix4d", [&] { hp_entry::eg_mul4(a, b, c); acc += c(0, 0); }},
           {"8x8 matrix * vector", [&] { hp_entry::eg_mv8(A, x, y); acc += y(0); }},
           {"dot of 256 doubles", [&] { acc += hp_entry::eg_dot(u, v); }},
           {"4x4 Cholesky solve", [&] { hp_entry::eg_solve4(spd, r, s); acc += s(0); }}});
  return acc == 1;
}
