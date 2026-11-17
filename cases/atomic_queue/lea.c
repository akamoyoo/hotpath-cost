// Latency of a dependent chain of lea: 3-part (disp+base+index*4) vs 2-part (base+index*4) vs 2-part (disp+base).
#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>
#define N 100000000L
#define CHAIN(name, ins) static double name(void) { uint64_t x = 1, b = 0; uint64_t t0 = __rdtsc(); \
  for (long i = 0; i < N; i++) { __asm__ volatile(ins ins ins ins ins ins ins ins ins ins : "+r"(x) : "r"(b)); } \
  return (double)(__rdtsc() - t0) / (N * 10.0); }
CHAIN(lea3, "leaq 0x80(%1,%0,4), %0\n\t")
CHAIN(lea2i, "leaq (%1,%0,4), %0\n\t")
CHAIN(lea2d, "leaq 0x80(%0), %0\n\t")
CHAIN(add1, "addq %1, %0\n\t")
int main(void) {
  for (int r = 0; r < 3; r++)
    printf("ticks per lea: 3-part %.2f  base+index*4 %.2f  disp+base %.2f  (add %.2f)\n", lea3(), lea2i(), lea2d(), add1());
}
