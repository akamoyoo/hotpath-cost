// Per-function signatures (README, "Per-function signatures"): `always` cases and their `may` neighbours.
// main calls every function, so the run-time check (tests/validate_always.py) sees each one return.
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <vector>
#define NI __attribute__((noinline))
static void sink(void* p) { asm volatile("" : : "r"(p) : "memory"); }
NI void* every_path(long n) { void* p = std::malloc(n + 1); sink(p); return p; }          // always alloc
NI void* one_branch(long n) { if (n > 3) { void* p = std::malloc(n); sink(p); return p; } return nullptr; }  // may
NI long loop_zero(long n) { long s = 0; for (long i = 0; i < n; i++) { void* p = std::malloc(8); sink(p); std::free(p); s++; } return s; }  // may
NI void* early_return(long n) { if (n == 0) return nullptr; void* p = std::malloc(n); sink(p); return p; }  // may
NI void push(std::vector<long>& v, long x) { v.push_back(x); }                          // may (growth)
NI void reserve_swap(std::vector<long>& v, long x) {                                     // Liquibook callback_now
    std::vector<long> w;
    w.reserve(16);
    w.push_back(x);
    v.swap(w);
}
NI long* make_one(long x) { return new long(x); }                                        // always alloc
NI long calls_maker(long x) { long* p = make_one(x); long r = *p; delete p; return r; }  // always alloc and free
NI long maybe_maker(long x) { if (x > 100) { long* p = make_one(x); long r = *p; delete p; return r; } return x; }  // may
struct Shape { virtual long area(long) = 0; virtual ~Shape() = default; };
struct Quiet : Shape { NI long area(long x) override { return x * 2; } };
struct Loud : Shape { NI long area(long x) override { long* p = new long(x); sink(p); long r = *p; delete p; return r; } };
NI long virt(Shape* s, long x) { return s->area(x); }                                    // may (one target allocates)
NI long both_loud(Loud* s, long x) { return s->area(x); }                                // always if resolved to Loud::area only
NI long throws_or_allocs(long x) { if (x < 0) throw std::runtime_error("neg"); void* p = std::malloc(8); sink(p); std::free(p); return x; }  // always: the throw path does not return
NI long catch_skips(long x) {                                                            // may: a caught exception skips the allocation
    try { if (x < -5) throw 1; } catch (int) { return 0; }
    void* p = std::malloc(8); sink(p); std::free(p); return x;
}
std::mutex mu;
long shared_v;
NI void locked(long x) { std::lock_guard<std::mutex> g(mu); shared_v += x; }             // always lock (pthread_mutex_lock)
NI long recurse(long n) { if (n <= 0) return 0; return recurse(n - 1) + 1; }              // never alloc
NI long rec_alloc(long n) { void* p = std::malloc(8); sink(p); std::free(p); return n <= 0 ? 0 : rec_alloc(n - 1); }  // always (own site)

// --- a fresh vector with reserve(n), n possibly zero: may. reserve(0) on an empty vector allocates nothing
// (capacity 0 is already enough); only n > 0 reaches operator new.
NI long reserve_n(long n) { std::vector<long> w; w.reserve(n); sink(w.data()); return (long)w.capacity(); }
// --- the Liquibook shape (OrderBook::callback_now in the analysed checkout): a local vector,
// reserve(other.capacity()), swap. may: the capacity may be zero. (In callback_now the reserve sits in
// `while (!callbacks_.empty())`, where the capacity cannot be zero, so each pass does allocate; proving
// that needs a value invariant of std::vector the analysis does not have. Book below is the shape of the
// current upstream: the working vector is a member, and the steady state allocates nothing.)
NI void reserve_capacity_swap(std::vector<long>& v) { std::vector<long> w; w.reserve(v.capacity()); w.swap(v); }
struct Book { std::vector<long> callbacks_, working_; bool handling_ = false; long sum = 0; NI void callback_now(); };
void Book::callback_now() {
    if (!handling_) {
        handling_ = true;
        while (!callbacks_.empty()) {
            working_.reserve(callbacks_.capacity());
            working_.swap(callbacks_);
            for (long x : working_) sum += x;
            working_.clear();
        }
        handling_ = false;
    }
}
// --- calls through pointers
NI long alloc_a(long x) { long* p = new long(x); sink(p); long r = *p; delete p; return r; }
NI long alloc_b(long x) { void* p = std::malloc(x + 1); sink(p); std::free(p); return x; }
// resolved by points-to to {alloc_a, alloc_b}, both always: always. With --precision type the same call is
// resolved by type matching, which is a fallback: may.
NI long call_fp(long (*fp)(long), long x) { return fp(x); }
// --- an interposable definition: its own claim stands; a caller's claim through it has evidence
// closed-world (it holds unless a strong definition elsewhere replaces the callee)
__attribute__((weak)) NI void* weak_alloc(long n) { void* p = std::malloc(n + 1); sink(p); return p; }
NI void* calls_weak(long n) { return weak_alloc(n); }
// --- a throw caught in the function: the exception object is allocated on every returning path
NI long caught_throw(long x) { try { throw x; } catch (long v) { return v; } }
// --- a callee that is always-alloc, invoked where a caught exception leads to a return: may (the callee's
// guarantee covers its normal returns only)
NI long invoke_caught(long x) { try { long* p = make_one(x); long r = *p; delete p; return r; } catch (...) { return 0; } }
// --- setjmp: control flow the CFG does not show; nothing is claimed
static jmp_buf jb;
NI long uses_setjmp(long x) { if (setjmp(jb)) return 1; void* p = std::malloc(8); sink(p); std::free(p); return x; }
// --- loops that may not terminate, a noreturn branch, a tail call
NI long spin_then_alloc(volatile long* f) { while (!*f) {} void* p = std::malloc(8); sink(p); std::free(p); return 1; }  // always
NI long do_while_alloc(volatile long* f) { long n = 0; do { void* p = std::malloc(8); sink(p); std::free(p); n++; } while (!*f); return n; }  // always
NI long abort_or_alloc(long x) { if (x < 0) std::abort(); void* p = std::malloc(8); sink(p); std::free(p); return x; }  // always
NI void* tail_maker(long x) { [[clang::musttail]] return every_path(x); }  // always
// --- a cycle cannot make itself always (least fixed point)
NI long cyc_b(long n);
NI long cyc_a(long n) { return cyc_b(n); }
NI long cyc_b(long n) { if (n <= 0) return 0; return cyc_a(n - 1) + 1; }
// --- other classes
NI long sys_write(long x) { return (long)::write(1, "", 0) + x; }                       // always syscall
std::atomic<long> counter;
NI long atom(long x) { return counter.fetch_add(x); }                                    // always atomic (own instruction)
int main(int argc, char**) {
    long s = 0;
    std::free(every_path(argc)); std::free(one_branch(argc)); s += loop_zero(argc); std::free(early_return(argc));
    std::vector<long> v; push(v, argc); reserve_swap(v, argc);
    s += calls_maker(argc) + maybe_maker(argc);
    Quiet q; Loud l; s += virt(argc > 5 ? (Shape*)&l : (Shape*)&q, argc) + virt(&l, argc) + both_loud(&l, argc);
    s += throws_or_allocs(argc) + catch_skips(argc);
    try { throws_or_allocs(-argc); } catch (...) {}
    locked(argc); s += recurse(argc) + rec_alloc(argc);
    s += reserve_n(argc) + reserve_n(0);
    std::vector<long> v2; reserve_capacity_swap(v2); v2.push_back(1); reserve_capacity_swap(v2);
    Book b; b.callback_now(); b.callbacks_.push_back(3); b.callback_now(); b.callbacks_.push_back(4); b.callback_now(); s += b.sum;
    s += call_fp(argc > 5 ? alloc_a : alloc_b, argc) + call_fp(alloc_a, argc);
    std::free(calls_weak(argc)); s += caught_throw(argc) + invoke_caught(argc) + uses_setjmp(argc);
    volatile long flag = 1; s += spin_then_alloc(&flag) + do_while_alloc(&flag) + abort_or_alloc(argc);
    std::free(tail_maker(argc)); s += cyc_a(argc) + sys_write(argc) + atom(argc);
    std::printf("%ld %zu\n", s, v.size());
}
