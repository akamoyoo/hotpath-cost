// Virtual dispatch while an object is under construction or destruction: the dynamic type is the base.
#include <cstdlib>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
struct Base { virtual void on(long x); Base(); virtual ~Base(); };
struct Loud : Base { void on(long x) override { keep = malloc(x + 8); } Loud() {} ~Loud() override {} };
struct Loud2 : Loud { void on(long x) override { keep = malloc(x + 16); } };
NI void fire(Loud* b, long x) { b->on(x); }               // static type Loud: targets Loud::on, Loud2::on -> always alloc
NI void fire_base(Base* b, long x) { b->on(x); }          // static type Base: includes Base::on
void Base::on(long x) { sink = x; }
NI Base::Base() { fire_base(this, 1); }
NI Base::~Base() { fire_base(this, 2); }
// virtual bases: during B's base-object constructor inside C, the final overrider of f seen through V* is V::f
struct V { virtual void f(long x) { sink = x; } virtual ~V() {} };
struct A : virtual V { void f(long x) override { keep = malloc(x + 8); } };
NI void fire_a(A* a, long x) { a->f(x); }                 // static type A: only A::f (and overriders in classes derived from A)
struct Hook { Hook(A* a) { fire_a(a, 3); } };
// E derives from A and does NOT override f; F derives from A, overrides without allocating but is only ever a base of G
struct E : A { Hook h; NI E() : h(this) {} };
// a class whose override is quiet, used only as a base: its own vtable is a construction-time vtable
struct QuietMid : A { void f(long x) override { sink = x; } Hook h; NI QuietMid() : h(this) {} };
struct Top : QuietMid { void f(long x) override { keep = malloc(x + 8); } NI Top() {} };
// static type with all concrete leaves loud, but an intermediate class under construction is quiet
struct S { virtual void g(long) = 0; virtual ~S() {} };
struct Mid : S { void g(long x) override { sink = x; }  NI Mid(); };
struct LeafA : Mid { void g(long x) override { keep = malloc(x + 8); } NI LeafA() {} };
NI void fire_mid(Mid* m, long x) { m->g(x); }
Mid::Mid() { fire_mid(this, 4); }
int main() { Loud l; fire(&l, 1); Loud2 l2; fire(&l2, 1); E e; Top t; LeafA a; fire_mid(&a, 1); fire_a(&t, 1); return 0; }
