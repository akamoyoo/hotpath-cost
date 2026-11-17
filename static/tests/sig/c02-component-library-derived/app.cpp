#include "sink.h"
#include <cstdlib>
void* volatile keep;
struct Quiet : Sink { long on(long x) override { return x + 1; } };
struct Loud : Sink { long on(long x) override { keep = malloc(x + 8); return x; } };
struct Loud2 : Loud { long on(long x) override { keep = malloc(x + 16); return x; } };
__attribute__((noinline)) long pull(Sink* s, long x) { return s->on(x); }          // module targets: Quiet::on, Loud::on, Loud2::on
__attribute__((noinline)) long pull_loud(Loud* s, long x) { return s->on(x); }     // module targets: Loud::on, Loud2::on  -> always alloc
struct QBase { virtual long get(long x) { return x; } virtual ~QBase() {} };       // every module override is quiet
struct Q2 : QBase { long get(long x) override { return x + 2; } };
QBase* make_q_from_library();
__attribute__((noinline)) long get(QBase* s, long x) { return s->get(x); }         // never alloc?
int main(int argc, char**) {
    Quiet q; Loud l; Loud2 l2; Q2 q2;
    long r = pull(&q, 1) + pull(&l, 1) + pull_loud(&l2, 1) + pull(make_from_library(0), 2) + get(&q2, 1) + get(make_q_from_library(), 3);
    return (int)r & 0;
}
