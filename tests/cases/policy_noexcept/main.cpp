// noexcept functions: which can let an exception escape (std::terminate at run time)?
#include <stdexcept>
#include <vector>
#define NOINL __attribute__((noinline))
NOINL int may_throw(int x) { if (x < 0) throw std::invalid_argument("neg"); return x; }
NOINL int caught_inside(int x) { try { return may_throw(x); } catch (...) { return 0; } }
NOINL int safe_leaf(int x) noexcept { return x * 2; }
NOINL int bad(int x) noexcept { return may_throw(x) + 1; }                  // escapes: may_throw throws
NOINL int at_in_noexcept(const std::vector<int>& v, int i) noexcept { return v.at(i); }   // escapes: out_of_range
NOINL int calls_noexcept(int x) noexcept { return safe_leaf(x) + caught_inside(x); }    // caught_inside catches: imprecise
NOINL int direct_throw(int x) noexcept { if (x == 7) throw 1; return x; }   // escapes
int main(int argc, char**) { std::vector<int> v(3); return safe_leaf(argc) + calls_noexcept(argc) + bad(argc) + at_in_noexcept(v, 0) + direct_throw(argc); }
