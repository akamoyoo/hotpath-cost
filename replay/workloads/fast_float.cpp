// fastfloat/fast_float: from_chars for doubles, the parsing step of a text market-data feed.
#include <fast_float/fast_float.h>
#include <cstring>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY double ff_parse(const char *b, const char *e) { double v = 0; fast_float::from_chars(b, e, v); return v; }
}
int main() {
  static const char *px = "187.25", *small = "1.2345678901234567e-5", *qty = "100", *longp = "4567.123456789012";
  double acc = 0;
  run_ops({{"price 187.25", [&] { acc += hp_entry::ff_parse(px, px + std::strlen(px)); }},
           {"integer 100", [&] { acc += hp_entry::ff_parse(qty, qty + std::strlen(qty)); }},
           {"17 digits, exponent", [&] { acc += hp_entry::ff_parse(small, small + std::strlen(small)); }},
           {"4567.123456789012", [&] { acc += hp_entry::ff_parse(longp, longp + std::strlen(longp)); }}});
  return acc == 1;
}
