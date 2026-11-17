// abseil/abseil-cpp: absl::flat_hash_map<uint64_t, uint64_t>, with the .cc files the map itself needs
// compiled in (see P in replay.py). Hash-table sampling (hashtablez) is not compiled into this build;
// raw_hash_set.cc still references it on its sampling paths, whose signatures change across versions
// and whose real definitions pull in absl::Mutex, flags and stack traces. The build links with those
// symbols unresolved (-Wl,--unresolved-symbols=ignore-all): none of the measured operations reaches
// them, and if one did, the run would crash and be recorded as a run failure.
#include "absl/container/flat_hash_map.h"
#include "hashmap_common.h"
int main() { return run_hashmap<absl::flat_hash_map<uint64_t, uint64_t>>(); }
