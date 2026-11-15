#include "sink.h"
#include <cstdlib>
#include <cstdint>
void* volatile keep;
struct Loud : Sink { void on(long x) override { keep = malloc(x + 8); } };
struct QuietSrc : Src { long get(long x) override { return x + 1; } };
__attribute__((noinline)) void dispatch(Sink* s, long x) { s->on(x); }                              // always alloc?
__attribute__((noinline)) void dispatch_tagged(uintptr_t t, long x) { ((Sink*)(t & ~uintptr_t(1)))->on(x); }  // tagged pointer: points-to falls back
__attribute__((noinline)) long pull(Src* s, long x) { return s->get(x); }                           // never alloc?
__attribute__((noinline)) long pull_tagged(uintptr_t t, long x) { return ((Src*)(t & ~uintptr_t(1)))->get(x); }
int main(int argc, char**) {
    Loud l; QuietSrc q;
    dispatch(&l, 1); dispatch(make_quiet(), 2);
    dispatch_tagged((uintptr_t)&l | 1, 1); dispatch_tagged((uintptr_t)make_quiet() | 1, 2);
    return (int)(pull(&q, 1) + pull(make_loud(), 2) + pull_tagged((uintptr_t)&q | 1, 1) + pull_tagged((uintptr_t)make_loud() | 1, 1)) & 0;
}
