// COLD-WRONG. Sites tagged cold that every normal run executes:
// 1. entry_tls: a thread_local with a dynamic initialiser; the entry is the per-request function of a
//    worker thread, and every worker allocates on its first request (no hint in the source).
// 2. entry_static: a function-local static (Clang's guard weight 1/1048576): the first call allocates.
// 3. entry_expect: a refill guarded by __builtin_expect(..., 0) that is taken on every 4th call (a
//    small buffer); the hint is the programmer's, but the run takes it all the time.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
struct Scratch { std::vector<long> v; Scratch() : v(1024) {} };
__attribute__((noinline)) long entry_tls(long x) { thread_local Scratch s; s.v[x & 1023] += x; return s.v[0]; }
__attribute__((noinline)) long entry_static(long x) { static std::string name(64 + (x & 1), 'n'); return (long)name.size(); }
long buf[4]; int used = 4;
__attribute__((noinline)) void refill() { long* p = (long*)std::malloc(4 * sizeof(long)); for (int i = 0; i < 4; i++) buf[i] = p[i] = i; asm volatile("" : : "r"(p) : "memory"); std::free(p); used = 0; }
__attribute__((noinline)) long entry_expect(long x) { if (__builtin_expect(used == 4, 0)) refill(); return buf[used++] + x; }
int main(int argc, char**) {
    long s = 0;
    std::vector<std::thread> ws;
    for (int t = 0; t < 2; t++) ws.emplace_back([&s, argc] { for (int i = 0; i < 3; i++) s += entry_tls(argc + i); });
    for (auto& w : ws) w.join();
    for (int i = 0; i < 3; i++) s += entry_static(argc);
    for (int i = 0; i < 12; i++) s += entry_expect(i);
    std::printf("%ld\n", s > 0 ? 1L : 0L);
}
