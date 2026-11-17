// gabime/spdlog (header-only): a synchronous logger on a null sink, and one on an ostream sink writing to a
// std::ostringstream that is cleared, so that formatting and the sink path both run.
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include <string>
#include "../drivers/hp_entry.h"
namespace hp_entry {
HP_ENTRY void sp_info(spdlog::logger &l, int id, double px) { l.info("order {} px {}", id, px); }
HP_ENTRY void sp_info_str(spdlog::logger &l, const std::string &s) { l.info("sym {}", s); }
HP_ENTRY void sp_debug_off(spdlog::logger &l, int id) { l.debug("dbg {}", id); }
}
#include "../bench_common.h"
int main() {
  auto ns = std::make_shared<spdlog::sinks::null_sink_mt>();
  spdlog::logger nl("null", ns);
  std::ostringstream os;
  auto ss = std::make_shared<spdlog::sinks::ostream_sink_mt>(os);
  spdlog::logger sl("ostream", ss);
  std::string s("ABCDEFGH");
  run_ops({{"null sink info(int, double)", [&] { hp_entry::sp_info(nl, 1, 2.5); }},
           {"null sink info(string)", [&] { hp_entry::sp_info_str(nl, s); }},
           {"null sink debug below level", [&] { hp_entry::sp_debug_off(nl, 1); }},
           {"ostream sink info(int, double)", [&] { hp_entry::sp_info(sl, 1, 2.5); if (os.tellp() > 1 << 20) os.str(""); }}});
}
