// "What counts as: the primitive was called". Program-defined functions whose NAME is a primitive's.
#include <cstddef>
#include <cstdio>
#include <new>
// (1) a user-space transport: the program's own extern "C" send/connect (POSIX names, not reserved by ISO C++ when
//     <sys/socket.h> is not included). No kernel entry, no libc wrapper.
static char ring[4096]; static unsigned head;
extern "C" __attribute__((noinline)) long send(int ch, const void* buf, unsigned long n, int) {
    const char* p = (const char*)buf; for (unsigned long i = 0; i < n; i++) ring[(head + i) & 4095] = p[i] + ch; head += n; return n; }
__attribute__((noinline)) long publish(int x) { return send(1, &x, sizeof x, 0); }          // claimed: always syscall?
// (2) replaced global operator new that never allocates (bump arena), and its delete
static char arena[1 << 16]; static size_t top;
void* operator new(size_t n) { void* p = arena + top; top += (n + 15) & ~size_t(15); return p; }
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
struct Node { long v; Node* next; };
__attribute__((noinline)) Node* mk(long v) { return new Node{v, nullptr}; }                  // claimed: always alloc?
__attribute__((noinline)) void rm(Node* n) { delete n; }                                     // claimed: always free?
// (3) a user placement form: operator new(size_t, Slab&) -- matches ^_Zn[wa][jm]
struct Slab { char b[256]; size_t t = 0; };
__attribute__((noinline)) void* operator new(size_t n, Slab& s) noexcept { void* p = s.b + s.t; s.t += n; return p; }
Slab slab;
__attribute__((noinline)) Node* mk_slab(long v) { return new (slab) Node{v, nullptr}; }      // claimed: always alloc?
// (4) class-specific operator new: _ZN1QnwEm, no pattern
struct Q { long v; static void* operator new(size_t) { static char b[64]; return b; } static void operator delete(void*) {} };
__attribute__((noinline)) Q* mk_q() { return new Q{1}; }
int main(int argc, char**) {
    long r = publish(argc); Node* n = mk(r); Node* s = mk_slab(r); Q* q = mk_q();
    volatile long keep = n->v + s->v + q->v; rm(n);
    return (int)(keep & 0);
}
