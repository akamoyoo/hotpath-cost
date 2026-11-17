// --pta-scope component: a local object holds `loud`; code of ANOTHER component replaces it with `quiet` before the
// call. The component solve must see the object as escaped. A wrong `always alloc` shows as a VIOLATION.
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
#include <memory>
#define NI __attribute__((noinline))
#define BIG sink = sink + 1; sink = sink + 2; sink = sink + 3; sink = sink + 4; sink = sink + 5; sink = sink + 6; sink = sink + 7; sink = sink + 8; sink = sink + 9; sink = sink + 10; sink = sink + 11; sink = sink + 12; sink = sink + 13; sink = sink + 14; sink = sink + 15; sink = sink + 16; sink = sink + 17; sink = sink + 18; sink = sink + 19; sink = sink + 20; sink = sink + 21; sink = sink + 22;
void* volatile keep; volatile long sink;
typedef void (*Fn)(long);
NI void loud(long x) { keep = malloc(x + 8); }
NI void quiet(long x) { sink = x; }
struct Ctx { Fn f; long pad[12]; };
// the other components (big enough not to be copied as "small callees": > 40 instructions, or recursive)
NI void big_setter(Ctx* c, long x) { BIG if (x) c->f = quiet; }
NI void rec_setter(Ctx* c, long n) { BIG if (n > 0) { rec_setter(c, n - 1); sink = sink + n; } else if (sink) c->f = quiet; }
NI void small_setter(Ctx* c) { BIG if (sink) c->f = quiet; }
NI void d1(Ctx* c) { small_setter(c); } NI void d2(Ctx* c) { d1(c); } NI void d3(Ctx* c) { d2(c); } NI void d4(Ctx* c) { d3(c); }
typedef void (*Setter)(Ctx*, long);
Setter g_setter = big_setter;                       // mutable global: fallback
struct Vis { virtual void visit(Ctx* c) { if (sink) c->f = quiet; } virtual ~Vis() {} };
struct Vis2 : Vis { void visit(Ctx* c) override { if (sink) c->f = quiet; sink = 1; } };
NI Vis* mkvis(long x) { return x ? new Vis : new Vis2; }
// 1 direct call into another component
NI void c_direct(long x) { Ctx c{}; c.f = loud; big_setter(&c, x); c.f(x); }
// 2 direct recursive callee
NI void c_rec(long x) { Ctx c{}; c.f = loud; rec_setter(&c, 2); c.f(x); }
// 3 chain of small callees deeper than the copy depth
NI void c_deep(long x) { Ctx c{}; c.f = loud; d4(&c); c.f(x); }
// 4 indirect call, resolved inside the component to one target that lives in another component
NI void c_indirect_local(long x) { Ctx c{}; c.f = loud; Setter volatile s = rec_setter; Setter t = s; t(&c, 2); c.f(x); }
// 5 indirect call through a mutable global (fallback)
NI void c_indirect_global(long x) { Ctx c{}; c.f = loud; g_setter(&c, x); c.f(x); }
// 6 virtual call (vtable rule) writing through its argument
NI void c_virtual(Vis* v, long x) { Ctx c{}; c.f = loud; v->visit(&c); c.f(x); }
// 7 the object reaches the other component through a global, not an argument
Ctx* g_ctx;
NI void big_global_setter(long x) { BIG if (x) g_ctx->f = quiet; }
NI void c_global(long x) { Ctx c{}; c.f = loud; g_ctx = &c; big_global_setter(x); c.f(x); }
// 8 through a returned pointer
Ctx the_ctx = {loud, {}};
NI Ctx* big_get(long x) { BIG if (x) the_ctx.f = quiet; return &the_ctx; }
NI void c_returned(long x) { Ctx* c = big_get(x); c->f(x); }
// 9 captured by a lambda that another component runs
NI void run_cb(const std::function<void()>& cb) { cb(); }
NI void c_lambda(long x) { Fn f = loud; run_cb([&f] { if (sink) f = quiet; }); f(x); }
// 10 a std::function member of an object, set in one component, called in another
struct Svc { std::function<void(long)> on = loud; };
NI void big_cfg(Svc* s, long x) { BIG if (x) s->on = quiet; }
NI void c_stdfn(long x) { Svc s; big_cfg(&s, x); s.on(x); }
// 11 container of handlers filled elsewhere
NI void fill(std::vector<Fn>& v) { BIG v.push_back(sink ? quiet : loud); }
NI void c_vector(long x) { std::vector<Fn> v; v.reserve(4); fill(v); v.back()(x); }
// 12 base pointers stored in one component, called in another
struct B { virtual void go(long) = 0; virtual ~B() {} };
struct BL : B { void go(long x) override { loud(x); } }; struct BQ : B { void go(long x) override { quiet(x); } };
std::unique_ptr<B> slot_;
NI void install(long x) { if (x) slot_.reset(new BQ); else slot_.reset(new BL); }
NI void c_base(long x) { slot_->go(x); }
// 13 out-parameter
NI void big_out(Fn* out, long x) { BIG if (x) *out = quiet; }
NI void c_out(long x) { Fn f = loud; big_out(&f, x); f(x); }
// 14 memcpy from an object another component filled
NI void big_fill(Ctx* c, long x) { BIG if (x) c->f = quiet; }
NI void c_memcpy(long x) { Ctx a{}, b{}; a.f = loud; b.f = loud; big_fill(&a, x); Ctx* volatile pa = &a; memcpy(&b, pa, sizeof b); b.f(x); }
// 15 control: nothing crosses
NI void c_control(long x) { Ctx c{}; c.f = loud; Ctx* volatile p = &c; p->f(x); }
int main(int argc, char**) {
    long one = argc; sink = 1; Vis* v = mkvis(one); install(one);
    c_direct(one); c_rec(one); c_deep(one); c_indirect_local(one); c_indirect_global(one); c_virtual(v, one); c_global(one);
    c_returned(one); c_lambda(one); c_stdfn(one); c_vector(one); c_base(one); c_out(one); c_memcpy(one); c_control(one);
    return 0;
}
