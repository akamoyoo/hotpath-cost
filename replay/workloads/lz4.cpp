// lz4/lz4 (lib/lz4.c compiled into this file): compress and decompress 4 KiB of fixed-width,
// market-data-like records.
#include "lz4.c"
#include <cstdio>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY int lz_compress(const char *s, char *d, int n, int cap) { return LZ4_compress_default(s, d, n, cap); }
HP_ENTRY int lz_decompress(const char *s, char *d, int n, int cap) { return LZ4_decompress_safe(s, d, n, cap); }
}
int main() {
  static char src[4096], dst[8192], back[4096]; int off = 0;
  for (int i = 0; off < 4000; i++) off += std::snprintf(src + off, sizeof src - off, "%08d,AAPL,%.2f,%d\n", 93000000 + i * 7, 187.25 + (i % 13) * 0.01, 100 * (1 + i % 9));
  int n = 4096, c = 0, acc = 0;
  run_ops({{"compress 4 KiB", [&] { c = hp_entry::lz_compress(src, dst, n, sizeof dst); }},
           {"decompress 4 KiB", [&] { acc += hp_entry::lz_decompress(dst, back, c, sizeof back); }}}, 200, 20000);
  return acc == 1;
}
