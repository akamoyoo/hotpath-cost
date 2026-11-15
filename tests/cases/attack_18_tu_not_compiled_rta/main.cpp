#include "shapes.hpp"
#include <cstdio>
__attribute__((noinline)) long entry(Base* b, long x) { return b->f(x) + 1; }
Quiet q;
int main(int argc, char**) {
    Base* b = make_loud(argc);
    std::printf("%ld %ld\n", entry(b, argc), entry(&q, argc));
}
