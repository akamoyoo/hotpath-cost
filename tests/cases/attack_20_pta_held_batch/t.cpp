// Pure C++ idioms that move code pointers around; in each, the allocating function is what runs.
#include <any>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <variant>
#include <vector>
#include <map>
#include <cstdint>
#define LOUD() do { void* p_ = std::malloc(48); asm volatile("" : : "r"(p_) : "memory"); std::free(p_); } while (0)
struct QuietF { long operator()(long x) const { return x; } };
struct LoudF { long operator()(long x) const { LOUD(); return x + 1; } };
std::variant<QuietF, LoudF> var;
__attribute__((noinline)) long e_variant(long x) { return std::visit([x](auto& f) { return f(x); }, var); }
std::any any_fn;
__attribute__((noinline)) long e_any(long x) { return std::any_cast<std::function<long(long)>&>(any_fn)(x); }
std::vector<std::function<long(long)>> fns;
__attribute__((noinline)) long e_vecfn(long x) { long s = 0; for (auto& f : fns) s += f(x); return s; }
std::function<long(long)> fa, fb;
__attribute__((noinline)) long e_swapfn(long x) { return fa(x); }
struct V { virtual long f(long x) { return x; } virtual ~V() {} };
struct A : virtual V { long f(long x) override { LOUD(); return x + 1; } };
struct B : virtual V { };
struct C : A, B { };
V* vobj;
__attribute__((noinline)) long e_vbase(long x) { return vobj->f(x); }
struct P { virtual P* clone() const = 0; virtual long run(long x) = 0; virtual ~P() = default; };
struct PL : P { PL* clone() const override { return new PL(*this); } long run(long x) override { LOUD(); return x; } };
struct PQ : P { PQ* clone() const override { return nullptr; } long run(long x) override { return x; } };
alignas(PL) unsigned char raw[sizeof(PL)];
P* placed;
__attribute__((noinline)) long e_placement(long x) { return placed->run(x); }
struct M { long a(long x) { return x; } long b(long x) { LOUD(); return x; } };
long (M::*pmf)(long) = &M::a;
__attribute__((noinline)) long e_pmf(M& m, long x) { return (m.*pmf)(x); }
std::map<int, long (*)(long)> handlers;
static long hq(long x) { return x; }
static long hl(long x) { LOUD(); return x; }
__attribute__((noinline)) long e_map(int k, long x) { return handlers.at(k)(x); }
struct Node { uintptr_t link; long (*fn)(long); };
Node nodes[2];
__attribute__((noinline)) long e_xorlist(long x) {   // XOR-linked list of nodes: walk two steps
    Node* prev = nullptr; Node* cur = &nodes[0]; long s = 0;
    for (int i = 0; i < 2 && cur; i++) { s += cur->fn(x); Node* nx = (Node*)(cur->link ^ (uintptr_t)prev); prev = cur; cur = nx; }
    return s;
}
int main(int argc, char**) {
    long x = argc;
    var = LoudF{};
    any_fn = std::function<long(long)>(LoudF{});
    fns.push_back(QuietF{}); fns.push_back([](long y) { LOUD(); return y; });
    fa = QuietF{}; fb = LoudF{}; std::swap(fa, fb);
    vobj = new C;
    placed = std::construct_at(reinterpret_cast<PL*>(raw));
    pmf = argc > 5 ? &M::a : &M::b; M m;
    handlers[1] = hq; handlers[2] = hl;
    nodes[0] = {(uintptr_t)&nodes[1], hq}; nodes[1] = {(uintptr_t)&nodes[0], hl};
    long s = e_variant(x) + e_any(x) + e_vecfn(x) + e_swapfn(x) + e_vbase(x) + e_placement(x) + e_pmf(m, x) + e_map(2, x) + e_xorlist(x);
    std::printf("%ld\n", s);
}
