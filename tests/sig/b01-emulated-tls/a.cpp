// A thread_local access compiled with -femulated-tls: code generation turns every access into a call of
// __emutls_get_address (libgcc / compiler-rt), which on a thread's first access calls pthread_once,
// pthread_mutex_lock/unlock and malloc/calloc. The IR shows only llvm.threadlocal.address.
#include <pthread.h>
#define NI __attribute__((noinline))
thread_local long counter = 5;
thread_local long big[64];
NI long bump(long x) { counter += x; return counter; }
NI long touch(long i, long x) { big[i & 63] += x; return big[(i + 1) & 63]; }
NI long outer(long x) { return bump(x) + touch(x, 1); }
static void* th(void*) { return (void*)outer(3); }
int main(int argc, char**) {
    pthread_t t; pthread_create(&t, nullptr, th, nullptr); pthread_join(t, nullptr);
    return (int)(outer(argc) & 0);
}
