// quill frontend with the default FrontendOptions (unbounded queue). One FrontendOptions type per
// process: quill keeps one thread context per thread, so the driver's BoundedDropping entries are not
// called here.
#define main driver_main
#include "main.cpp"
#undef main
#include "../bench_common.h"
namespace hp_entry {
HP_ENTRY void quill_log_default_str(quill::Logger *l, const std::string &s) { LOG_INFO(l, "sym {}", s); }
HP_ENTRY void quill_log_default_debug(quill::Logger *l, int id) { LOG_DEBUG(l, "dbg {}", id); }
}
int main() {
  quill::Backend::start();
  auto s1 = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("c1");
  quill::Logger *l = quill::Frontend::create_or_get_logger("root", std::move(s1));
  quill::Frontend::preallocate();
  std::string s("ABCDEFGH");
  run_ops({{"LOG_INFO int, double", [&] { hp_entry::quill_log_default(l, 1, 2.0); }},
           {"LOG_INFO std::string", [&] { hp_entry::quill_log_default_str(l, s); }},
           {"LOG_DEBUG below level", [&] { hp_entry::quill_log_default_debug(l, 1); }}}, 10000, 100000);
}
