// abseil/abseil-cpp: absl::flat_hash_map<uint64_t, uint64_t>, with the .cc files the map itself needs
// compiled in (see P in replay.py). Hash-table sampling (hashtablez) is not compiled into this build:
// raw_hash_set.cc still references its functions on the sampling paths, whose signatures change across
// versions and whose definitions pull in absl::Mutex, flags and stack traces. They are left undefined
// ("unresolved_ok" in P); none of the measured operations reaches them. Versions that enabled sampling by
// default are built with ABSL_CONSUME_DLL, which turns it off; the few where that does not work leave its
// per-thread counter undefined, and fail to build rather than run with it at address 0.
#include "absl/container/flat_hash_map.h"
#include "hashmap_common.h"
int main() { return run_hashmap<absl::flat_hash_map<uint64_t, uint64_t>>(); }
