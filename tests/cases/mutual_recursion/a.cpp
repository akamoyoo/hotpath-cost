// Mutual recursion across translation units, and self recursion on a tree.
#include "../../fx_test.h"
bool is_odd(unsigned n);
FX_NOINLINE bool is_even(unsigned n) FX_NB { return n == 0 ? true : is_odd(n - 1); }
struct Node { Node* l; Node* r; };
FX_NOINLINE int depth(const Node* n) FX_NB { if (!n) return 0; int a = depth(n->l), b = depth(n->r); return 1 + (a > b ? a : b); }
FX_NOINLINE int flat(int x) FX_NB { return x * 3; }
int main(int argc, char**) { Node c{nullptr, nullptr}, b{&c, nullptr}, a{&b, &c}; return is_even(10 + argc) + depth(&a) + flat(argc); }
