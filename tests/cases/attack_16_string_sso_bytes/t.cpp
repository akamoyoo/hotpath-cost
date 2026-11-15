// The std::string pointer models (bodies: true, self) copy the bytes reachable from their
// arguments into a *fresh heap buffer*. A short string lives in its small buffer, inside the string
// object: copying it (assignment, copy construction, swap) moves the bytes inside the objects, not
// into a fresh buffer. Test pta_string_bytes covers 64-byte (heap) strings only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using Fn = long (*)(long);
__attribute__((noinline)) long quiet(long x) { return x + 1; }
__attribute__((noinline)) long loud(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
Fn volatile seed = loud;
std::string a(8, '\0'), b, c(8, '\0'), d;
Fn fb = quiet, fd = quiet;
__attribute__((noinline)) void fill() {
    Fn f = seed;
    std::memcpy(&a[0], &f, sizeof f);   // 8 bytes: small buffer
    b = a;                              // copy assignment
    std::memcpy(&c[0], &f, sizeof f);
    d.swap(c);                          // swap of two short strings
}
__attribute__((noinline)) long entry_copy(long x) { std::memcpy(&fb, b.data(), sizeof fb); return fb(x); }
__attribute__((noinline)) long entry_swap(long x) { std::memcpy(&fd, d.data(), sizeof fd); return fd(x); }
int main(int argc, char**) { fill(); std::printf("%ld %ld\n", entry_copy(argc), entry_swap(argc)); }
