// Calls through pointers. In each via_*: the source "obviously" stores loud, and at run time the slot holds quiet,
// put there through a channel the points-to analysis has to model. A wrong `always alloc` shows as a VIOLATION.
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <csignal>
#include <atomic>
#include <functional>
#include <thread>
#include <pthread.h>
#include <emmintrin.h>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
typedef void (*Fn)(long);
NI void loud(long x) { keep = malloc(x + 8); }
NI void quiet(long x) { sink = x; }
NI void quiet2(int) { sink = 2; }
// 1 global set by a dynamic initializer
Fn g_ctor = loud; struct Init { Init() { g_ctor = quiet; } } init_;
NI void via_ctor(long x) { g_ctor(x); }
// 2 set by another thread
Fn g_thr = loud; NI void via_thread(long x) { g_thr(x); }
// 3 byte-wise copy of a struct
struct H { long tag; Fn f; };
H h_src = {1, quiet}, h_dst = {2, loud};
NI void bytecopy(void* d, const void* s, size_t n) { auto* a = (unsigned char*)d; auto* b = (const unsigned char*)s; for (size_t i = 0; i < n; i++) a[i] = b[i]; }
NI void via_bytecopy(long x) { h_dst.f(x); }
// 4 xor swap through integers
Fn xa = loud, xb = quiet;
NI void xorswap() { uintptr_t a = (uintptr_t)xa, b = (uintptr_t)xb; a ^= b; b ^= a; a ^= b; xa = (Fn)a; xb = (Fn)b; }
NI void via_xor(long x) { xa(x); }
// 5 atomic exchange / compare-exchange
std::atomic<Fn> at{loud};
NI void via_atomic(long x) { at.load()(x); }
// 6 previous handler returned by signal()
NI void via_signal(long x) { signal(SIGUSR1, quiet2); auto prev = signal(SIGUSR1, SIG_DFL); Fn f = loud; if (prev != SIG_ERR) f = (Fn)(void*)prev == nullptr ? f : f; ((void (*)(int))prev)((int)x); f = nullptr; }
// 7 pthread key
pthread_key_t key;
NI void via_tsd(long x) { ((Fn)pthread_getspecific(key))(x); }
// 8 varargs
NI void va_call(long x, ...) { va_list ap; va_start(ap, x); Fn f = va_arg(ap, Fn); va_end(ap); f(x); }
NI void via_va(long x) { va_call(x, x ? quiet : loud); }
// 9 thrown and caught
NI void thrower(long x) { throw (Fn)(x ? quiet : loud); }
NI void via_throw(long x) { try { thrower(x); } catch (Fn f) { f(x); } }
// 10 alias of the global
extern "C" { Fn hookvar = loud; extern Fn hookalias __attribute__((alias("hookvar"))); }
NI void set_alias() { hookalias = quiet; }
NI void via_alias(long x) { hookvar(x); }
// 11 SIMD shuffle of a pair
Fn pair2[2] = {quiet, loud};
NI void simdswap() { __m128i v = _mm_loadu_si128((const __m128i*)pair2); v = _mm_shuffle_epi32(v, 0x4e); _mm_storeu_si128((__m128i*)pair2, v); }
NI void via_simd(long x) { pair2[1](x); }
// 12 thread_local
thread_local Fn tl = loud;
NI void via_tls(long x) { tl(x); }
// 13 std::function reassigned
std::function<void(long)> sf = loud;
NI void via_stdfn(long x) { sf(x); }
// 14 member pointer to data
struct Obj { Fn a, b; }; Obj obj = {loud, loud};
NI void set_mp(Obj* o, Fn Obj::* m, Fn v) { o->*m = v; }
NI void via_memptr(long x) { obj.b(x); }
// 15 qsort moving handlers
struct E { long k; Fn f; }; E tab[2] = {{2, loud}, {1, quiet}};
static int cmp(const void* a, const void* b) { return (int)(((const E*)a)->k - ((const E*)b)->k); }
NI void via_qsort(long x) { tab[0].f(x); }
// 16 container_of arithmetic
struct Outer { Fn f; long pad; long member; }; Outer outer_ = {loud, 0, 0};
NI void set_outer(long* m, Fn v) { ((Outer*)((char*)m - offsetof(Outer, member)))->f = v; }
NI void via_container_of(long x) { outer_.f(x); }
// 17 heap object, realloc moves it
H* hp;
NI void via_realloc(long x) { hp->f(x); }
// 18 local, exact: constant pointer (true always) -- control
NI void via_const(long x) { Fn f = loud; Fn volatile* p = &f; (*p)(x); }
// 19 struct returned in registers
struct R2 { Fn f; long n; };
NI R2 mk(long x) { return R2{x ? quiet : loud, x}; }
NI void via_ret(long x) { mk(x).f(x); }
// 20 pointer chosen by index into a mutable table written elsewhere
Fn table[4] = {loud, loud, loud, loud};
NI void poke(Fn* t, long i, Fn v) { t[i] = v; }
NI void via_table(long x) { table[x & 3](x); }
int main(int argc, char**) {
    long one = argc;
    std::thread([] { g_thr = quiet; tl = quiet; }).join();
    bytecopy(&h_dst, &h_src, sizeof h_src); xorswap();
    Fn exp = loud; at.compare_exchange_strong(exp, quiet);
    pthread_key_create(&key, nullptr); pthread_setspecific(key, (void*)quiet);
    set_alias(); simdswap(); tl = quiet; sf = quiet; set_mp(&obj, &Obj::b, quiet);
    qsort(tab, 2, sizeof(E), cmp); set_outer(&outer_.member, quiet);
    hp = (H*)malloc(sizeof(H)); hp->f = loud; H tmp = {0, quiet}; hp = (H*)realloc(hp, 1 << 20); memcpy(hp, &tmp, sizeof tmp);
    poke(table, one & 3, quiet);
    via_ctor(one); via_thread(one); via_bytecopy(one); via_xor(one); via_atomic(one); via_signal(one); via_tsd(one); via_va(one);
    via_throw(one); via_alias(one); via_simd(one); via_tls(one); via_stdfn(one); via_memptr(one); via_qsort(one);
    via_container_of(one); via_realloc(one); via_const(one); via_ret(one); via_table(one);
    return 0;
}
