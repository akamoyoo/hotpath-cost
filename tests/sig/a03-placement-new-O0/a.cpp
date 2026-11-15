// fn-effects -O 0: the standard placement operator new(size_t, void*) (_ZnwmPv, an inline function of <new>) is a real call in the IR.
// The policy excludes it from the alloc *symbols* (re:^_Zn[wa][jm]Pv$) -- is it excluded from the alloc *primitives* too?
#include <new>
#include <cstdlib>
#include <cstddef>
#define NI __attribute__((noinline))
struct P { long a, b; };
alignas(P) unsigned char buf[sizeof(P)];
alignas(P) unsigned char bufs[4 * sizeof(P)];
NI P* place(long x) { return ::new (buf) P{x, x}; }                   // placement new: no allocation
NI P* place_arr(long x) { return ::new (bufs) P[4]; }                 // _ZnamPv
NI void* nothrow_new(long x) { return ::operator new(16, std::nothrow); }   // really allocates (control)
int main(int argc, char**) { P* p = place(argc); P* q = place_arr(argc); void* r = nothrow_new(argc); return (int)(p->a + q->a) & 0 & (r != nullptr); }
