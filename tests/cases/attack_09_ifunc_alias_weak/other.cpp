#include <cstdlib>
extern "C" long hookw(long x) { void* p = std::malloc(48); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 9; }
