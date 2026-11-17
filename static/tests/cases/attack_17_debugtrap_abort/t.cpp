// abi.intrinsics_no_call lists llvm.debugtrap, and the abort class has llvm.trap and llvm.ubsantrap
// but not llvm.debugtrap. __builtin_debugtrap() is an int3: without a debugger attached the process
// is killed by SIGTRAP (default action: terminate and dump core), as surely as by __builtin_trap's ud2.
// Here a "checked" path in a hot function uses it as its assertion (as some engines do in release
// builds to stop under a debugger when one is attached).
#include <cstdio>
#define CHECK(c) do { if (!(c)) __builtin_debugtrap(); } while (0)
long table[8];
__attribute__((noinline)) long entry(long i) { CHECK(i < 8); return table[i & 7] + i; }
int main(int argc, char**) { std::printf("%ld\n", entry(argc + 8)); return 0; }
