// abseil/abseil-cpp: absl::flat_hash_map<uint64_t, uint64_t>, with the .cc files the map itself needs
// compiled in (see P in replay.py). Hash-table sampling (hashtablez) is not compiled into this build;
// raw_hash_set.cc still references ForcedTrySample() on its forced-sampling path, and the real one
// pulls in absl::Mutex, flags and stack traces. The definition below returns an unsampled handle, which
// is what sampling-disabled builds do; none of the measured operations reaches it.
#include "absl/container/flat_hash_map.h"
#include "absl/container/internal/hashtablez_sampler.h"
#include "hashmap_common.h"
namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {
HashtablezInfoHandle ForcedTrySample(const HashtableTypeInfo &) { return HashtablezInfoHandle(); }
}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl
int main() { return run_hashmap<absl::flat_hash_map<uint64_t, uint64_t>>(); }
