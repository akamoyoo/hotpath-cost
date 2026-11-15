// glibc's clock_getcpuclockid always validates the clock with a clock_getres system call
// (syscall 229). The libc summary that fn-effects loads (bin/summaries/libc.so.6.json) says syscall none.
#include <time.h>
#include <cstdio>
#include <sys/types.h>
__attribute__((noinline)) long entry(pid_t pid) {
    clockid_t c;
    return clock_getcpuclockid(pid, &c) == 0 ? (long)c : -1L;
}
int main(int argc, char**) { std::printf("%ld\n", entry(argc > 5 ? 1 : 0)); }
