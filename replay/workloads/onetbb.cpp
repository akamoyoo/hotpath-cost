// oneapi-src/oneTBB: concurrent containers and a task group, on the calling thread: a concurrent_queue
// push and try_pop, a concurrent_hash_map insert and erase and find (100,000 keys), and a task_group
// run and wait with two threads at most. The library's sources are compiled in (srcs_glob in replay.py).
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>
#include <oneapi/tbb/task_group.h>
#include <oneapi/tbb/global_control.h>
#include <cstdint>
#include <vector>
#include "../drivers/hp_entry.h"
#include "../workload.h"
using Map = tbb::concurrent_hash_map<uint64_t, uint64_t>;
namespace hp_entry {
HP_ENTRY int q_push_pop(tbb::concurrent_queue<int> &q, int x) { q.push(x); int v = 0; q.try_pop(v); return v; }
HP_ENTRY void m_insert_erase(Map &m, uint64_t k) { { Map::accessor a; m.insert(a, k); a->second = k; } m.erase(k); }
HP_ENTRY bool m_find(const Map &m, uint64_t k) { Map::const_accessor a; return m.find(a, k); }
HP_ENTRY void tg_run_wait(tbb::task_group &g, int *x) { g.run([x] { ++*x; }); g.wait(); }
}
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
int main() {
  tbb::global_control gc(tbb::global_control::max_allowed_parallelism, 2);
  tbb::concurrent_queue<int> q; Map m; const long n = 100000; std::vector<uint64_t> keys(n);
  for (long i = 0; i < n; i++) { keys[i] = mix(i + 1); Map::accessor a; m.insert(a, keys[i]); }
  tbb::task_group g; int x = 0; size_t i = 0, hits = 0; uint64_t fresh = 1ULL << 63;
  run_ops({{"concurrent_queue push+try_pop", [&] { x += hp_entry::q_push_pop(q, 1); }},
           {"concurrent_hash_map insert+erase", [&] { hp_entry::m_insert_erase(m, fresh + i++); }},
           {"concurrent_hash_map find (hit)", [&] { hits += hp_entry::m_find(m, keys[i++ % n]); }},
           {"task_group run+wait", [&] { hp_entry::tg_run_wait(g, &x); }}});
  return (x + hits) == 1;
}
