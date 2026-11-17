// odygrd/quill v13.1.0: the frontend ("hot frontend") LOG_ macros, with the default queue
// (UnboundedBlocking) and with the BoundedDropping queue that docs/frontend_options.rst says
// "has a fixed capacity and never reallocates". The backend is linked (so that this is a
// complete program) but is not an entry: the README calls it the cold backend.
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include <cstdint>
#include <string>
#include <utility>
#include "../hp_entry.h"

struct BDOptions : quill::FrontendOptions {
  static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
  static constexpr size_t initial_queue_capacity = 1u << 20;
};
using BDFrontend = quill::FrontendImpl<BDOptions>;
using BDLogger = quill::LoggerImpl<BDOptions>;

namespace hp_entry {
HP_ENTRY void quill_log_default(quill::Logger *l, int id, double px) { LOG_INFO(l, "order {} px {}", id, px); }
HP_ENTRY void quill_log_bd(BDLogger *l, int id, double px) { LOG_INFO(l, "order {} px {}", id, px); }
HP_ENTRY void quill_log_bd_str(BDLogger *l, const std::string &s) { LOG_INFO(l, "sym {}", s); }
HP_ENTRY void quill_log_bd_cstr(BDLogger *l, const char *s, uint64_t q) { LOG_INFO(l, "sym {} qty {}", s, q); }
HP_ENTRY void quill_log_bd_debug_off(BDLogger *l, int id) { LOG_DEBUG(l, "dbg {}", id); }   // below the logger's level at run time
}

int main() {
  quill::Backend::start();
  auto s1 = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("c1");
  quill::Logger *l = quill::Frontend::create_or_get_logger("root", std::move(s1));
  auto s2 = BDFrontend::create_or_get_sink<quill::ConsoleSink>("c2");
  BDLogger *b = BDFrontend::create_or_get_logger("bd", std::move(s2));
  BDFrontend::preallocate();
  hp_entry::quill_log_default(l, 1, 2.0); hp_entry::quill_log_bd(b, 1, 2.0);
  hp_entry::quill_log_bd_str(b, std::string("x")); hp_entry::quill_log_bd_cstr(b, "x", 1);
  hp_entry::quill_log_bd_debug_off(b, 1);
  return 0;
}
