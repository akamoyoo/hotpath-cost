// The uncontended fast path of atomic_queue's push as GCC emits it at HEAD (lock xadd; index remap;
// lea; lock cmpxchg), with the element address formed by a 3-part lea or by a 2-part lea, as in cf9ab816.
// Own code: the instruction sequence copied from the disassembly, run on private memory, one thread.
#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>
static uint32_t q[2048] __attribute__((aligned(64)));
#define N 20000000L
#define RUN(name, ADDR) static double name(void) { uint64_t t0 = __rdtsc(); \
  for (long i = 0; i < N; i++) { uint64_t a, c, d; \
    __asm__ volatile("movl $1, %%eax\n\tlock xaddl %%eax, (%[p])\n\tmovl $0x404, %%edx\n\tbzhil %%edx, %%eax, %%ecx\n\t" \
      "bextrl %%edx, %%eax, %%edx\n\tandl $0x300, %%eax\n\tshll $4, %%ecx\n\torl %%edx, %%eax\n\torl %%eax, %%ecx\n\t" \
      ADDR "\n\tmovl (%%rdx), %%eax\n\tmovl %%eax, %%ecx\n\tlock cmpxchgl %%ecx, (%%rdx)\n\t" \
      : "=&a"(a), "=&c"(c), "=&d"(d) : [p] "r"(q), [p80] "r"(q + 32) : "memory", "cc"); } \
  return (double)(__rdtsc() - t0) / N; }
RUN(lea3, "leaq 0x80(%[p],%%rcx,4), %%rdx")
RUN(lea2, "leaq (%[p80],%%rcx,4), %%rdx")
int main(void) { for (int r = 0; r < 3; r++) printf("ticks per push: 3-part lea %.2f   2-part lea %.2f\n", lea3(), lea2()); }
