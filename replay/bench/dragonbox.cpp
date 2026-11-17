// jk-jeon/dragonbox: shortest round-trip decimal for a double (the algorithm only, header-only), the
// step of printing a price.
#include <dragonbox/dragonbox.h>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
namespace hp_entry {
HP_ENTRY long long db_decimal(double x) { auto r = jkj::dragonbox::to_decimal(x); return (long long)r.significand + r.exponent; }
}
int main() {
  long long acc = 0; volatile double a = 187.25, b = 0.1 + 0.2, c = 1.2345678901234567e-5;
  run_ops({{"to_decimal(187.25)", [&] { acc += hp_entry::db_decimal(a); }},
           {"to_decimal(0.1 + 0.2)", [&] { acc += hp_entry::db_decimal(b); }},
           {"to_decimal(1.2345678901234567e-5)", [&] { acc += hp_entry::db_decimal(c); }}});
  return acc == 1;
}
