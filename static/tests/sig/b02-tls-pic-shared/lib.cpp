// A -fPIC shared object with a thread_local: general-dynamic TLS, every access calls __tls_get_addr, which for a
// dlopen'ed module allocates the thread's block on first access (malloc) under the loader's TLS lock.
#define NI __attribute__((noinline))
thread_local long counter = 5;
static thread_local long hidden_counter = 7;
extern "C" NI long bump(long x) { counter += x; return counter; }
extern "C" NI long bump_local(long x) { hidden_counter += x; return hidden_counter; }
namespace { NI long inner(long x) { hidden_counter ^= x; return hidden_counter; } }
extern "C" NI long outer(long x) { return inner(x) + 1; }
