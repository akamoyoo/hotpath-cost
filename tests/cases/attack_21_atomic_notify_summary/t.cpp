// C++20 std::atomic<long>::notify_all(). With the libstdc++ Clang links here (the toolchain's GCC
// installation), notify_all calls std::__detail::__notify_impl in libstdc++.so, which tail-calls
// syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX) when a thread waits. The library summary
// (made by the binary analyser, which loses system calls behind a tail call to syscall()) says
// __notify_impl makes no system call and takes no lock.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
std::atomic<long> flag{0};   // not futex-sized: notified through the library's waiter pool
__attribute__((noinline)) void entry() { flag.store(1, std::memory_order_release); flag.notify_all(); }
int main() {
    std::thread t([] { flag.wait(0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    entry();
    t.join();
    std::printf("%d\n", flag.load());
}
