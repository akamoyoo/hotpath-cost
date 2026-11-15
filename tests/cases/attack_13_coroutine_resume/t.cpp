// C++20 coroutine: main creates the coroutine (frame allocated by operator new); the entry resumes it;
// the code after the first suspension point allocates.
#include <coroutine>
#include <cstdio>
#include <cstdlib>
struct Task {
    struct promise_type {
        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h;
};
Task work(long n) {
    co_await std::suspend_always{};
    void* p = std::malloc(n * 10); asm volatile("" : : "r"(p) : "memory"); std::free(p);
    co_return;
}
std::coroutine_handle<> current;
__attribute__((noinline)) void entry() { current.resume(); }
int main(int argc, char**) {
    Task t = work(argc);
    current = t.h;
    entry();      // runs to the co_await
    entry();      // runs the allocation
    std::printf("%d\n", (int)t.h.done());
    t.h.destroy();
}
