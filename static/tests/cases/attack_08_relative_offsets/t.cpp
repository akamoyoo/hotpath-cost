// A table of 32-bit offsets of handlers relative to an anchor function (as relative vtables and
// position-independent dispatch tables do). A 32-bit integer carries no provenance, so
// anchor + offset has the provenance of the anchor alone: the points-to set is {anchor}, not UNK,
// and the call is resolved to the anchor. The README says a reassembly through inttoptr is covered
// by UNK; here it is not.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using Fn = long (*)(long);
__attribute__((noinline)) long anchor(long x) { return x; }
__attribute__((noinline)) long h_quiet(long x) { return x + 1; }
__attribute__((noinline)) long h_loud(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
int32_t rel[2];
__attribute__((noinline)) void init_table() {
    rel[0] = (int32_t)((intptr_t)&h_quiet - (intptr_t)&anchor);
    rel[1] = (int32_t)((intptr_t)&h_loud - (intptr_t)&anchor);
}
__attribute__((noinline)) long entry(unsigned i, long x) {
    Fn f = (Fn)((intptr_t)&anchor + rel[i & 1]);
    return f(x);
}
int main(int argc, char**) { init_table(); std::printf("%ld\n", entry(argc, argc)); }
