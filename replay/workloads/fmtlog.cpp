// MengRao/fmtlog (header-only, with the fmt version its submodule pins): the logging call on the hot
// thread, which formats into a thread-local queue; a background thread polls it to a file.
#include "fmtlog.h"
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY void fl_info(int id, double px) { logi("order {} px {}", id, px); }
HP_ENTRY void fl_debug_off(int id) { logd("dbg {}", id); }
}
int main() {
  fmtlog::setLogFile("/dev/null", false); fmtlog::setLogLevel(fmtlog::INF);
  fmtlog::startPollingThread(1000);
  run_ops({{"logi(int, double)", [&] { hp_entry::fl_info(1, 2.5); }},
           {"logd below level", [&] { hp_entry::fl_debug_off(1); }}}, 10000, 100000);
  fmtlog::stopPollingThread();
}
