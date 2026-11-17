// taskflow/taskflow (header-only): submit a prebuilt one-task taskflow to an executor with one worker
// and wait for it. Counted on the submitting thread (the worker's own allocations are not counted).
#include <taskflow/taskflow.hpp>
#include <atomic>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
namespace hp_entry {
HP_ENTRY void tf_run_wait(tf::Executor &e, tf::Taskflow &f) { e.run(f).wait(); }
HP_ENTRY void tf_async_wait(tf::Executor &e, std::atomic<long> &x) { e.async([&x] { x++; }).wait(); }
}
int main() {
  tf::Executor ex(1); tf::Taskflow flow; std::atomic<long> x{0};
  flow.emplace([&x] { x++; });
  run_ops({{"run(taskflow of 1 task).wait()", [&] { hp_entry::tf_run_wait(ex, flow); }},
           {"async(task).wait()", [&] { hp_entry::tf_async_wait(ex, x); }}}, 1000, 20000);
  return x == 1;
}
