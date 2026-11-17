// key functions: the vtables of Quiet and Loud are emitted here (in the module fn-effects analyses)
#include "shapes.hpp"
#include <cstdlib>
Base::~Base() {}
long Quiet::f(long x) { return x + 1; }
long Loud::f(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
