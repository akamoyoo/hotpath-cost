// Regressions (round 3) for library pointer models: qsort hands its comparator pointers to any element;
// strdup and strcpy copy bytes, which may be pointer bytes.
#include "../../fx_test.h"
#include <cstdlib>
#include <cstring>
#include <vector>
std::vector<int>* volatile g_log;
using Fn = int (*)(int);
FX_NOINLINE int harmless(int x) { return x + 1; }
FX_NOINLINE int alloc_cmp(int x) { g_log = new std::vector<int>(2); return x; }
FX_NOINLINE int alloc_dup(int x) { g_log = new std::vector<int>(3); return x; }
struct Item { long key; Fn f; };
Item g_items[2] = {{2, harmless}, {1, alloc_cmp}};
FX_NOINLINE int cmp(const void* a, const void* b) {
  const Item* x = (const Item*)a;
  const Item* y = (const Item*)b;
  return x->f((int)x->key) - y->f((int)y->key);
}
FX_NOINLINE void sort_items() FX_NB { std::qsort(g_items, 2, sizeof(Item), cmp); }
char g_raw[16];
char* volatile g_dup;
FX_NOINLINE void prepare() { Fn f = alloc_dup; std::memcpy(g_raw, &f, sizeof f); g_raw[8] = 0; g_dup = strdup(g_raw); }
FX_NOINLINE int via_strdup(int x) FX_NB { Fn f; std::memcpy(&f, g_dup, sizeof f); return f(x); }
int main(int argc, char**) { prepare(); sort_items(); return g_items[0].f(argc) + via_strdup(argc); }
