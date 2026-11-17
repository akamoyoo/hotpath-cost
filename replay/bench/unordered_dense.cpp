// martinus/unordered_dense: ankerl::unordered_dense::map<uint64_t, uint64_t>
#include <ankerl/unordered_dense.h>
#include "hashmap_common.h"
int main() { return run_hashmap<ankerl::unordered_dense::map<uint64_t, uint64_t>>(); }
