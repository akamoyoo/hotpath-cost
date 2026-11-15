// Calls C++ makes without a call expression, and instructions that become library calls. Every function here does
// something of a hazard class at run time; the check lists any that is `never` (any evidence) for the class observed.
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>
#include <compare>
#include <tuple>
#include <typeinfo>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
struct Loud { long v; Loud(long x) : v(x) { keep = malloc(8); } ~Loud() { free(keep); } };
struct Guard { Guard() { pthread_mutex_lock(&mu); } ~Guard() { pthread_mutex_unlock(&mu); } };
NI long by_value(Loud l) { return l.v; }                                    // destructor of a by-value parameter (callee side on this ABI? caller)
NI long temp(long x) { return by_value(Loud(x)) + 1; }                       // temporary: ctor + dtor
struct Conv { long v; NI operator long() const { keep = malloc(v + 8); return v; } };
NI long conversion(const Conv& c) { long x = c; return x; }
NI long defarg_src() { keep = malloc(16); return 3; }
NI long with_default(long x, long y = defarg_src()) { return x + y; }
NI long default_argument(long x) { return with_default(x); }
struct Cmp { long v; std::strong_ordering operator<=>(const Cmp& o) const { Guard g; return v <=> o.v; } };
NI bool spaceship(const Cmp& a, const Cmp& b) { return a < b; }
NI long static_local(long x) { static Loud l(x); return l.v; }               // __cxa_guard_acquire (lock class) + ctor + atexit
thread_local Loud tl_obj(7);
NI long tls_wrapper(long x) { return tl_obj.v + x; }                         // _ZTW wrapper -> init -> ctor, __cxa_thread_atexit
struct SB { long a, b; template <std::size_t I> long get() const { keep = malloc(8); return I ? b : a; } };
namespace std { template <> struct tuple_size<SB> : integral_constant<size_t, 2> {}; template <size_t I> struct tuple_element<I, SB> { using type = long; }; }
NI long structured(const SB& s) { auto [a, b] = s; return a + b; }
struct It { long i; long operator*() const { return i; } It& operator++() { ++i; Guard g; return *this; } bool operator!=(const It& o) const { return i != o.i; } };
struct Range { It begin() const { return {0}; } It end() const { return {3}; } };
NI long range_for(const Range& r) { long s = 0; for (long v : r) s += v; return s; }
NI long lambda(long x) { auto f = [x] { keep = malloc(x + 8); return x; }; return f(); }
struct Member { Loud l{1}; }; struct Holder { Member m; long z = 0; };
NI long defaulted(long x) { Holder h; Holder h2 = h; return h2.z + x; }      // defaulted ctor/copy/dtor run Loud's
NI long new_array(long n) { long* p = new long[n]; long v = p[0]; delete[] p; return v & 0; }   // length check: __cxa_throw_bad_array_new_length
struct PB { virtual ~PB() {} virtual long f() { return 1; } }; struct PD : PB { long f() override { return 2; } };
NI long dyn_ref(PB& b) { return dynamic_cast<PD&>(b).f(); }                  // __cxa_bad_cast on failure
NI const char* type_id(PB* b) { return typeid(*b).name(); }                  // __cxa_bad_typeid on null
struct Wide { long a, b, c; }; std::atomic<Wide> aw;
NI long atomic_wide(long x) { Wide w = aw.load(); w.a += x; aw.store(w); return w.a; }          // libatomic: locks
struct __attribute__((packed)) Packed { char c; std::atomic<long> a; }; Packed pk;
NI long atomic_misaligned(long x) { return pk.a.fetch_add(x); }
std::atomic<__int128> a128;
NI long atomic_128(long x) { __int128 v = a128.load(); a128.store(v + x); return (long)a128.exchange(v); }
NI long once(long x) { static std::once_flag f; std::call_once(f, [] { keep = malloc(4); }); return x; }
NI long noexcept_term(long x) noexcept { if (x == 12345) throw 1; return x; }
NI __int128 div128(__int128 a, __int128 b) { return a / b; }
NI long double ldmath(long double x) { return std::fmod(x, 3.0L) + std::sqrt(x); }
NI long big_copy(char* d, const char* s, long n) { memcpy(d, s, n); return n; }
NI long vla(long n) { char buf[n + 1]; buf[n] = 0; char* volatile p = buf; return p[n]; }
int main(int argc, char**) {
    long one = argc; Conv c{one}; Cmp a{1}, b{2}; SB s{1, 2}; Range r; PD d; char x[64], y[64] = {0};
    sink = temp(one) + conversion(c) + default_argument(one) + spaceship(a, b) + static_local(one) + tls_wrapper(one) + structured(s) + range_for(r)
         + lambda(one) + defaulted(one) + new_array(one) + dyn_ref(d) + (long)type_id(&d)[0] + atomic_wide(one) + atomic_misaligned(one) + atomic_128(one)
         + once(one) + noexcept_term(one) + (long)div128(100, one) + (long)ldmath(one) + big_copy(x, y, 40 + one) + vla(one);
    return 0;
}
