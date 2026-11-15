// C++20 std::counting_semaphore, std::latch and std::atomic::wait in libstdc++ are header code that
// blocks in syscall(SYS_futex, ...). The hot-path lock class lists sem_wait and condition variables,
// but these semaphores never reach a pthread or sem_* function: lock comes out none.
#include <semaphore>
#include <latch>
#include <thread>
#include <atomic>
#include <cstdio>
std::counting_semaphore<8> sem(0);
std::latch done(1);
std::atomic<int> flag{0};
__attribute__((noinline)) long entry_sem() { sem.acquire(); return 1; }      // blocks until main releases
__attribute__((noinline)) long entry_latch() { done.wait(); return 2; }
__attribute__((noinline)) long entry_wait() { flag.wait(0); return 3; }
int main() {
    std::thread t([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); sem.release(); done.count_down();
                       std::this_thread::sleep_for(std::chrono::milliseconds(300)); flag.store(1); flag.notify_all(); });
    long s = entry_sem() + entry_latch() + entry_wait();
    t.join();
    std::printf("%ld\n", s);
}
