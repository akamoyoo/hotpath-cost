#include "shapes.hpp"
#include <cstdlib>
int* volatile g_keep;
static int alloc_hook(int x) { g_keep = (int*)std::malloc(8); return x; }
int (*g_hook)(int) = nullptr;
struct Installer { Installer() { g_hook = alloc_hook; } } g_installer;       // global constructor
void fill_holder(Holder& h) {
  h.fp = [](int x) { g_keep = (int*)std::malloc(16); return x; };           // captureless lambda -> pointer
  long pad[8] = {1};
  h.cb = [pad](int x) { g_keep = (int*)std::malloc(32); return x + (int)pad[0]; };
}
