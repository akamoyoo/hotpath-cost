// Cyan4973/xxHash, header-only (XXH_INLINE_ALL): XXH3 64-bit and XXH64 over 16, 64 and 1024 bytes.
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
namespace hp_entry {
HP_ENTRY unsigned long long xx3(const void *p, size_t n) { return XXH3_64bits(p, n); }
HP_ENTRY unsigned long long xx64(const void *p, size_t n) { return XXH64(p, n, 0); }
}
int main() {
  static unsigned char buf[1024]; for (int i = 0; i < 1024; i++) buf[i] = (unsigned char)(i * 131 + 7);
  unsigned long long acc = 0;
  run_ops({{"XXH3_64bits 16 B", [&] { acc += hp_entry::xx3(buf, 16); }},
           {"XXH3_64bits 64 B", [&] { acc += hp_entry::xx3(buf, 64); }},
           {"XXH3_64bits 1024 B", [&] { acc += hp_entry::xx3(buf, 1024); }},
           {"XXH64 64 B", [&] { acc += hp_entry::xx64(buf, 64); }},
           {"XXH64 1024 B", [&] { acc += hp_entry::xx64(buf, 1024); }}});
  return acc == 1;
}
