// A code address that arrives as an integer without provenance (here printed with %p and parsed
// back by the program's own code, as an address read from a config or another process would be) and
// is stored into a function-pointer variable with memcpy. No inttoptr is involved: the store writes
// an i64 with no provenance into the slot, and a later load of the pointer sees only what the slot
// held before (the initial `quiet`), instead of UNK.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
using Fn = long (*)(long);
__attribute__((noinline)) long quiet(long x) { return x + 1; }
__attribute__((noinline)) long loud(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
Fn hook = quiet;
Fn hook2 = quiet;
// a value array of doubles (NaN-boxing style): a code pointer kept as a double, unboxed with memcpy
double vals[4];
Fn unboxed = quiet;
__attribute__((noinline)) uint64_t parse_hex(const char* s) {
    uint64_t v = 0;
    if (s[0] == '0' && s[1] == 'x') s += 2;
    for (; *s; s++) v = v * 16 + (uint64_t)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
    return v;
}
__attribute__((noinline)) long entry_memcpy(long x) { return hook(x); }
__attribute__((noinline)) long entry_cast(long x) { return hook2(x); }
__attribute__((noinline)) long entry_double(long x) { std::memcpy(&unboxed, &vals[x & 3], sizeof unboxed); return unboxed(x); }
int main(int argc, char**) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%p", (void*)&loud);
    uint64_t v = parse_hex(buf);
    std::memcpy(&hook, &v, sizeof hook);       // object representation copy
    hook2 = reinterpret_cast<Fn>(v);           // inttoptr: should be UNK
    Fn l = loud;
    for (auto& d : vals) std::memcpy(&d, &l, sizeof d);   // box
    std::printf("%ld %ld %ld\n", entry_memcpy(argc), entry_cast(argc), entry_double(argc));
}
