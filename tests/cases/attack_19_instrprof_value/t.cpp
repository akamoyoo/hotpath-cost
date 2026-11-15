// -fprofile-instr-generate (a PGO training build): every indirect call gets value profiling, which
// calls the profile runtime's __llvm_profile_instrument_target; on the first record for a call site it
// allocates the site's value nodes with calloc. The analysed IR shows llvm.instrprof.* intrinsics,
// which abi.intrinsics_no_call lists as never calling.
#include <cstdio>
__attribute__((noinline)) long a(long x) { return x + 1; }
__attribute__((noinline)) long b(long x) { return x + 2; }
long (*volatile hook)(long) = a;
__attribute__((noinline)) long entry(long x) { return hook(x); }
int main(int argc, char**) { if (argc > 3) hook = b; std::printf("%ld\n", entry(argc)); }
