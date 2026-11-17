// martinus/robin-hood-hashing: robin_hood::unordered_map<uint64_t, uint64_t>, which every version has
// (newer versions also name the flat variant unordered_flat_map, and unordered_map chooses it for small
// keys and values like these).
// Older versions of robin_hood.h rely on standard headers arriving through other headers, which newer
// standard libraries no longer do; they are included here first.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <robin_hood.h>
#include "hashmap_common.h"
int main() { return run_hashmap<robin_hood::unordered_map<uint64_t, uint64_t>>(); }
