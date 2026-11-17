// greg7mdp/parallel-hashmap: phmap::flat_hash_map<uint64_t, uint64_t>
#include <parallel_hashmap/phmap.h>
#include "hashmap_common.h"
int main() { return run_hashmap<phmap::flat_hash_map<uint64_t, uint64_t>>(); }
