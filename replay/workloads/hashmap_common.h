// Shared operations for the hash-map workloads: a map of 100,000 random 64-bit keys, then
//   find (hit)        look up a present key
//   find (miss)       look up an absent key
//   insert+erase      insert a new key and erase it again, at constant size
//   operator[] (hit)  increment the value of a present key
#pragma once
#include "../drivers/hp_entry.h"
#include "../workload.h"
#include <cstdint>
#include <vector>

static inline uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; return x ^ (x >> 33); }

namespace hp_entry {
template <class M> HP_ENTRY bool hm_find(const M &m, uint64_t k) { return m.find(k) != m.end(); }
template <class M> HP_ENTRY void hm_insert_erase(M &m, uint64_t k) { m.emplace(k, k); m.erase(k); }
template <class M> HP_ENTRY void hm_index(M &m, uint64_t k) { m[k] += 1; }
}

template <class M> int run_hashmap() {
  M m; const long n = 100000;
  std::vector<uint64_t> keys(n);
  for (long i = 0; i < n; i++) { keys[i] = mix(i + 1); m.emplace(keys[i], i); }
  size_t i = 0, hits = 0; uint64_t fresh = 1ULL << 63;
  run_ops({{"find (hit)", [&] { hits += hp_entry::hm_find(m, keys[i++ % n]); }},
           {"find (miss)", [&] { hits += hp_entry::hm_find(m, mix(fresh + i++)); }},
           {"insert+erase", [&] { hp_entry::hm_insert_erase(m, mix(fresh + 7 + i++)); }},
           {"operator[] (hit)", [&] { hp_entry::hm_index(m, keys[i++ % n]); }}});
  return hits == 1;
}
