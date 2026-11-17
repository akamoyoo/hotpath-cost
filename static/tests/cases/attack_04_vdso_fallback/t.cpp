// The README: "With the libc summary loaded, the [vDSO] fallback shows up as syscall as well."
// CLOCK_PROCESS_CPUTIME_ID is never served by the vDSO: clock_gettime always enters the kernel.
// Also timespec_getres/timespec_get (C11) and pthread_getcpuclockid + clock_gettime on it.
#include <time.h>
#include <pthread.h>
#include <cstdio>
__attribute__((noinline)) long entry_cpu() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_nsec; }
__attribute__((noinline)) long entry_thread() { clockid_t c; pthread_getcpuclockid(pthread_self(), &c); timespec t; clock_gettime(c, &t); return t.tv_nsec; }
int main() { std::printf("%d\n", (int)((entry_cpu() + entry_thread()) > 0)); }
