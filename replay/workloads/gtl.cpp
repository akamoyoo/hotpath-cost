// greg7mdp/gtl: gtl::flat_hash_map<uint64_t, uint64_t>
#include <gtl/phmap.hpp>
#include "hashmap_common.h"
int main() { return run_hashmap<gtl::flat_hash_map<uint64_t, uint64_t>>(); }
