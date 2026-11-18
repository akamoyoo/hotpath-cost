// boostorg/unordered: boost::unordered_map<uint64_t, uint64_t>, the node-based container with the
// std::unordered_map interface; its history goes back to 2008. The commit's own include/ comes first;
// the other Boost modules come from one installed Boost release (BOOST_INC in replay.py).
#include <cstdint>
#include <cstddef>
#include <boost/unordered_map.hpp>
#include "hashmap_common.h"
int main() { return run_hashmap<boost::unordered_map<uint64_t, uint64_t>>(); }
