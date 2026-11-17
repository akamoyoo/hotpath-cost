// HowardHinnant/date (date.h, header-only part): parse a market-data timestamp with microseconds and
// format one, the usual way through streams.
#include <date/date.h>
#include <chrono>
#include <sstream>
#include <string>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
using us_time = date::sys_time<std::chrono::microseconds>;
namespace hp_entry {
HP_ENTRY long long dt_parse(const std::string &s) {
  std::istringstream in(s); us_time t; in >> date::parse("%Y-%m-%d %H:%M:%S", t); return t.time_since_epoch().count();
}
HP_ENTRY size_t dt_format(us_time t) { return date::format("%Y-%m-%d %H:%M:%S", t).size(); }
HP_ENTRY int dt_civil(int days) { auto ymd = date::year_month_day(date::sys_days(date::days(days))); return int(ymd.year()) + unsigned(ymd.month()); }
}
int main() {
  std::string s = "2026-11-17 09:30:00.123456"; us_time t = us_time(std::chrono::microseconds(1763371800123456LL));
  long long acc = 0; int d = 20000;
  run_ops({{"parse with istringstream", [&] { acc += hp_entry::dt_parse(s); }},
           {"format to std::string", [&] { acc += (long long)hp_entry::dt_format(t); }},
           {"days to year_month_day", [&] { acc += hp_entry::dt_civil(d++ % 40000); }}}, 1000, 50000);
  return acc == 1;
}
