// Round 5, rule R1 (provenance) and its strict-aliasing refinement: every way below forges a code
// pointer from integers; each call must fall back (alloc not a plain none). The last entry checks the
// precision the refinement keeps: an int field next to a function pointer does not taint it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Fn = long (*)(long);
__attribute__((noinline)) long quiet(long x) { return x + 1; }
__attribute__((noinline)) long loud(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
union Pun { uint64_t bits; Fn fn; };
Pun pun = {0};
struct Halves { uint32_t lo, hi; } halves;
Fn from_halves = quiet;
struct Rec { int count; Fn handler; };
Rec rec = {0, quiet};
Fn shifted = quiet;
__attribute__((noinline)) uint64_t hide(Fn f) { uint64_t v; std::memcpy(&v, &f, 8); return v ^ 0x5a5a5a5aULL; }
__attribute__((noinline)) void setup(int argc) {
    pun.bits = hide(loud) ^ 0x5a5a5a5aULL;                       // union punning: written as an integer
    uint64_t v = hide(loud) ^ 0x5a5a5a5aULL;
    halves.lo = (uint32_t)v; halves.hi = (uint32_t)(v >> 32);   // two 32-bit halves
    std::memcpy(&from_halves, &halves, 8);
    rec.count = argc;                                            // an int beside the pointer
    uintptr_t a = (uintptr_t)&quiet;
    a += (uintptr_t)&loud - (uintptr_t)&quiet;                   // arithmetic on a function address
    std::memcpy(&shifted, &a, 8);
}
__attribute__((noinline)) long e_union(long x) { return pun.fn ? pun.fn(x) : x; }
__attribute__((noinline)) long e_halves(long x) { return from_halves(x); }
__attribute__((noinline)) long e_arith(long x) { return shifted(x); }
__attribute__((noinline)) long e_field(long x) { return rec.handler(x); }
int main(int argc, char**) {
    setup(argc);
    std::printf("%ld\n", e_union(argc) + e_halves(argc) + e_arith(argc) + e_field(argc));
}
