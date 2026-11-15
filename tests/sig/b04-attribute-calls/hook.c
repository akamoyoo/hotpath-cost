/* A tracer's entry hook, in another object of the link (what a build with -mfentry links against). */
#include <unistd.h>
__attribute__((no_instrument_function)) void __fentry__(void) {
    __asm__ volatile("push %rdi; push %rsi; push %rdx; push %rcx; push %rax; push %r8; push %r9; push %r10; push %r11");
    static const char m[] = "t\n"; write(2, m, 2);
    __asm__ volatile("pop %r11; pop %r10; pop %r9; pop %r8; pop %rax; pop %rcx; pop %rdx; pop %rsi; pop %rdi");
}
