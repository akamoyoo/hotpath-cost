/* Uses a GCC nested function: Clang does not compile this file. */
#include <stdlib.h>
int legacy_checksum(const unsigned char* p, int n) {
  int acc = 0;
  void add(int x) { acc += x; }
  for (int i = 0; i < n; i++) add(p[i]);
  if (n > 1000) { void* q = malloc(n); free(q); }
  return acc;
}
