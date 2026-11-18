// boostorg/unordered: boost::unordered_flat_map<uint64_t, uint64_t> (open addressing, added in 2022).
// The commit's own include/ comes first; the other Boost modules it needs come from one installed
// Boost release (BOOST_INC in replay.py).
#include <cstdint>
#include <cstddef>
#include <boost/unordered/unordered_flat_map.hpp>
#include "hashmap_common.h"
int main() { return run_hashmap<boost::unordered_flat_map<uint64_t, uint64_t>>(); }
