// Normal return and post-dominance. Every function is driven so that it returns normally WITHOUT allocating
// whenever the source allows it; a wrong `always alloc` shows as a VIOLATION.
#include <cstdlib>
#include <cstdio>
#include <csetjmp>
#include <ucontext.h>
#include <stdexcept>
#include <pthread.h>
#include <unistd.h>
#define NI __attribute__((noinline))
volatile long sink; void* volatile keep;
NI void A(long n = 24) { keep = malloc(n); }               // always alloc
NI void thrower(long x) { if (x) throw std::runtime_error("t"); }
NI void quiet() { sink = sink + 1; }
// 1 irreducible flow with goto
NI void irreducible(long x) { long i = 0; if (x & 1) goto L2;
L1: i++; if (i > 3) goto out; quiet();
L2: i += 2; if (i > 5) goto out; if (x & 2) A(); goto L1;
out: sink = i; }
// 2 computed goto
NI void computed(long x) { static void* tab[] = {&&a, &&b, &&c}; goto *tab[x % 3];
a: A(); goto c; b: quiet(); c: sink = x; }
// 3 switch into a loop (Duff)
NI void duff(long n) { long i = (n + 3) / 4; switch (n % 4) { case 0: do { A(); [[fallthrough]]; case 3: quiet(); [[fallthrough]]; case 2: quiet(); [[fallthrough]]; case 1: quiet(); } while (--i > 0); } }
// 4 infinite loop with a side exit before the allocation
NI void side_exit(long x) { for (;;) { if (x == 7) return; x = (x * 5 + 1) & 15; if (x == 3) break; } A(); }
// 5 unreachable / assume on the other branch (never taken at run time: UB otherwise)
NI void unreach(long x) { if (x == 12345) __builtin_unreachable(); if (x > 0) return; A(); }
NI void assume_(long x) { [[assume(x != 777)]]; if (x & 1) A(); }
// 6 exit / quick_exit / abort / pthread_exit in a callee on the other path
NI void die_if(long x) { if (x == 99) exit(0); }
NI void exit_branch(long x) { if (x) { die_if(x); return; } A(); }
// 7 exceptions: the Must block skipped, caught inside f
NI void catch_skip(long x) { try { thrower(x); A(); } catch (...) { quiet(); } }
NI void catch_then_alloc(long x) { try { thrower(x); } catch (...) { quiet(); } A(); }           // true always
struct D { ~D() { quiet(); } };
NI void dtor_unwind(long x) { try { D d; thrower(x); A(); } catch (const std::exception&) { } }
NI void callee_always_invoked(long x) { try { D d; A(); thrower(x); } catch (...) { } }          // true always (A first)
NI void maker_throws(long x) { thrower(x); A(); }                                                // always (throw is not a return)
NI void wraps_maker(long x) { try { maker_throws(x); } catch (...) { } }                         // invoke: landing pad returns -> may
NI void noexcept_wrap(long x) noexcept { maker_throws(x); }                                      // always (terminate otherwise)
struct Ctor { void* p; Ctor(long x) try : p((thrower(x), malloc(8))) { } catch (...) { quiet(); } };  // ctor try-block rethrows
NI void uses_ctor(long x) { try { Ctor c(x); keep = c.p; } catch (...) { } }
NI void fn_try(long x) try { thrower(x); A(); } catch (...) { quiet(); }                         // function-try-block of a plain function returns
// 8 builtin setjmp/longjmp (no returns_twice attribute on the intrinsic), jmp back into f from a callee
void* jb[5];
NI void jumper(long x) { if (x) __builtin_longjmp(jb, 1); A(); }                                 // always alloc
NI void bsetjmp(long x) { if (__builtin_setjmp(jb)) { quiet(); return; } jumper(x); }
// 9 ucontext: getcontext is not a returns_twice function for clang; setcontext from a callee
ucontext_t uc; volatile int uflag;
NI void ctx_jumper() { if (!uflag) { uflag = 1; setcontext(&uc); } A(); }                         // always alloc
NI void ctx(long) { uflag = 0; getcontext(&uc); if (uflag) { quiet(); return; } ctx_jumper(); }
NI void ctx2(long) { volatile int once = 0; getcontext(&uc); quiet(); if (!once) { once = 1; uflag = 0; ctx_jumper(); } }
// 10 real setjmp in a callee that is inlined into f
static inline __attribute__((always_inline)) int sj(jmp_buf b) { return 0; }
jmp_buf jb2;
NI void lj(long x) { if (x) longjmp(jb2, 1); A(); }
NI void real_setjmp(long x) { if (setjmp(jb2) == 0) lj(x); }
// 11 musttail
NI void mt_callee(long x) { if (x) return; A(); }
NI void mt(long x) { [[clang::musttail]] return mt_callee(x); }
// 12 loop that may run zero times, do-while that runs once
NI void loop0(long n) { for (long i = 0; i < n; i++) A(); }
NI void loop1(long n) { long i = 0; do { A(); } while (++i < n); }                               // true always
// 13 recursion: least fixed point
NI void rb(long n); NI void ra(long n) { if (n > 0) rb(n - 1); } NI void rb(long n) { if (n > 0) ra(n - 1); }
NI void rec_base(long n) { if (n == 0) return; rec_base(n - 1); A(); }
NI void rc(long n); NI void rd(long n) { rc(n); } NI void rc(long n) { if (n <= 0) { quiet(); return; } rd(n - 1); }  // each "allocates" only via the other: never
// 14 thread exit in the middle
NI void* th(void* p) { if (p) pthread_exit(nullptr); A(); return nullptr; }
// 15 conditional operator / short circuit / select
NI void sc(long x) { (x > 0) && (A(), true); }
NI void sel(long x) { void* p = x ? nullptr : malloc(8); keep = p; }
// 16 malloc(0), realloc(p,0)
NI void m0() { keep = malloc(0); }
int main(int argc, char** argv) {
    long one = argc, zero = argc - 1;
    irreducible(1); irreducible(0); computed(1); computed(2); duff(3); duff(2); duff(1); side_exit(7); unreach(5); assume_(2);
    exit_branch(1); catch_skip(one); catch_then_alloc(one); dtor_unwind(one); callee_always_invoked(one); wraps_maker(one);
    try { maker_throws(one); } catch (...) { }
    uses_ctor(one); fn_try(one); bsetjmp(one); ctx(0); ctx2(0); real_setjmp(one); mt(one); loop0(zero); loop1(zero);
    ra(3); rec_base(0); rc(4); sc(zero); sel(one); m0(); noexcept_wrap(zero);
    pthread_t t; pthread_create(&t, nullptr, th, &t); pthread_join(t, nullptr);
    return 0;
}
