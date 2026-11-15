// The hot-path policy's `known` list (no class) has the pattern re:^pthread_.*attr_ , meant for the
// pthread_attr_* setters and getters. It also matches pthread_getattr_np, which on the main thread
// opens and reads /proc/self/maps (fopen: malloc, the stdio lock, openat/read system calls) and
// allocates the cpuset buffer. `known` is consulted before the libc summary.
#include <pthread.h>
#include <cstdio>
__attribute__((noinline)) long entry() {
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return -1;
    void* lo; size_t sz;
    pthread_attr_getstack(&a, &lo, &sz);
    pthread_attr_destroy(&a);
    return (long)sz;
}
int main() { std::printf("%ld\n", entry() > 0 ? 1L : 0L); }
