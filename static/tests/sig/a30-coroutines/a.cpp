// Coroutines: the ramp function "returns" at the first suspension; the rest runs in resume(), called by someone else.
#include <coroutine>
#include <cstdlib>
#include <cstdio>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
NI void A() { keep = malloc(24); }
// a task whose frame comes from a static pool: promise-specific operator new (no global operator new)
struct Task {
    struct promise_type {
        static void* operator new(std::size_t n) { static char pool[8][512]; static int k; return pool[k++ & 7]; }
        static void operator delete(void*) {}
        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {} void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> h;
};
struct Eager {
    struct promise_type {
        static void* operator new(std::size_t n) { static char pool[8][512]; static int k; return pool[k++ & 7]; }
        static void operator delete(void*) {}
        Eager get_return_object() { return Eager{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {} void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> h;
};
struct Gate { bool ready; bool await_ready() const noexcept { return ready; } void await_suspend(std::coroutine_handle<>) const noexcept {} void await_resume() const noexcept {} };
NI Task lazy_alloc(long x) { A(); co_return; }                         // body: always alloc; the ramp returns before it
NI Eager eager_then_await(long x) { co_await Gate{x == 0}; A(); co_return; }   // suspends before A() when x != 0
NI Eager eager_alloc_first(long x) { A(); co_await Gate{false}; co_return; }   // true always (A before the first suspension)
// a generator: the consumer stops early
struct Gen {
    struct promise_type {
        long v;
        static void* operator new(std::size_t n) { static char pool[8][512]; static int k; return pool[k++ & 7]; }
        static void operator delete(void*) {}
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(long x) noexcept { v = x; return {}; }
        void return_void() {} void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> h;
};
NI Gen counter(long n) { for (long i = 0; i < n; i++) co_yield i; A(); }
NI long take_one(long n) { Gen g = counter(n); g.h.resume(); long v = g.h.promise().v; g.h.destroy(); return v; }   // never reaches A()
NI long drive(long x) { Task t = lazy_alloc(x); /* never resumed */ t.h.destroy(); Eager e = eager_then_await(x); e.h.destroy(); Eager f = eager_alloc_first(x); f.h.destroy(); return take_one(3); }
int main(int argc, char**) { sink = drive(argc); return 0; }
