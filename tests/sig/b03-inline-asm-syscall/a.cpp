// System calls made by inline assembly that the policy's pattern "(syscall|sysenter|int $0x80)" does not match,
// and control transfers that "(call|jmp|ret|...)" does not match.
#define NI __attribute__((noinline))
// baseline: matched
NI long sys_plain(long nr) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory"); return r; }
// (1) upper-case mnemonic (GNU as and LLVM's assembler accept it)
NI long sys_upper(long nr) { long r; __asm__ volatile("SYSCALL" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory"); return r; }
// (2) the instruction as bytes (how code for old assemblers spells it)
NI long sys_bytes(long nr) { long r; __asm__ volatile(".byte 0x0f, 0x05" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory"); return r; }
// (3) int $0x80 written in decimal (the i386 ABI entry also works in 64-bit processes: getpid is 20 there)
NI long sys_int128(long nr) { long r; __asm__ volatile("int $128" : "=a"(r) : "a"(nr) : "memory"); return r; }
// (4) a call out of the block that is not spelled call/jmp: push a return address, conditional jump to a function
extern "C" NI void asm_target() { __asm__ volatile("syscall" ::"a"(39L) : "rcx", "r11", "memory"); }
NI long cond_jump_call(long x) {
    __asm__ volatile("leaq 1f(%%rip), %%rax\n\tpushq %%rax\n\ttestq %0, %0\n\tjnz asm_target\n\taddq $8, %%rsp\n1:" : : "r"(x) : "rax", "rcx", "r11", "memory", "cc");
    return x; }
int main(int argc, char**) {
    long a = sys_plain(39), b = sys_upper(39), c = sys_bytes(39), d = sys_int128(20), e = cond_jump_call(argc);
    return (a == b && b == c && c == d && e) ? 0 : 1;
}
