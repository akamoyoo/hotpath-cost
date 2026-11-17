// fmtlib/fmt (header-only): formatting into a reused memory_buffer, and fmt::format of short strings.
#define FMT_HEADER_ONLY 1
#include <fmt/format.h>
#include <iterator>
#include <string>
#include "../drivers/hp_entry.h"
namespace hp_entry {
HP_ENTRY size_t fmt_to_buf(fmt::memory_buffer &b, int id, double px) { b.clear(); fmt::format_to(std::back_inserter(b), "order {} px {}", id, px); return b.size(); }
HP_ENTRY size_t fmt_short(int id) { return fmt::format("{}", id).size(); }
HP_ENTRY size_t fmt_double(double px) { return fmt::format("{:.4f}", px).size(); }
HP_ENTRY size_t fmt_long(int id, double px) { return fmt::format("order {} px {} side buy venue XNAS", id, px).size(); }
}
#include "../bench_common.h"
int main() {
  fmt::memory_buffer b; size_t acc = 0;
  run_ops({{"format_to(memory_buffer)", [&] { acc += hp_entry::fmt_to_buf(b, 12345, 101.25); }},
           {"format int (short string)", [&] { acc += hp_entry::fmt_short(12345); }},
           {"format double {:.4f}", [&] { acc += hp_entry::fmt_double(101.25); }},
           {"format long string", [&] { acc += hp_entry::fmt_long(12345, 101.25); }}});
  return acc == 1;
}
