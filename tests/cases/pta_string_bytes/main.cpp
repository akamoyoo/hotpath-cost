// Soundness doubt of round 2: the std::string model ("self-store"). Code-pointer bytes are copied into one
// string's buffer, the string is copied (assignment, swap), and the bytes are read back from the other.
#include "../../fx_test.h"
#include <cstring>
#include <string>
#include <vector>
std::vector<int>* volatile g_log;
using Fn = int (*)(int);
FX_NOINLINE int harmless(int x) { return x + 1; }
FX_NOINLINE int alloc_str(int x) { g_log = new std::vector<int>(5); return x; }
FX_NOINLINE int alloc_swap(int x) { g_log = new std::vector<int>(6); return x; }
FX_NOINLINE int alloc_append(int x) { g_log = new std::vector<int>(7); return x; }
std::string g_e(64, '\0'), g_f;
Fn volatile g_seed3 = alloc_append;
std::string g_a(64, '\0'), g_b, g_c(64, '\0'), g_d;
Fn volatile g_seed = alloc_str, g_seed2 = alloc_swap;
FX_NOINLINE void fill() {
  Fn f = g_seed;
  std::memcpy(&g_a[0], &f, sizeof f);   // pointer bytes into a heap string buffer
  g_b = g_a;                            // copied by the string's own code
  Fn h = g_seed2;
  std::memcpy(&g_c[0], &h, sizeof h);
  g_d.swap(g_c);                        // exchanged between two strings
  Fn k = g_seed3;
  std::memcpy(&g_e[0], &k, sizeof k);
  g_f.append(g_e.data(), g_e.size());   // copied by an out-of-line string member (_M_append)
}
FX_NOINLINE int via_string(int x) FX_NB { Fn f; std::memcpy(&f, g_b.data(), sizeof f); return f(x); }
FX_NOINLINE int via_swap(int x) FX_NB { Fn f; std::memcpy(&f, g_d.data(), sizeof f); return f(x); }
FX_NOINLINE int via_append(int x) FX_NB { Fn f; std::memcpy(&f, g_f.data(), sizeof f); return f(x); }
int main(int argc, char**) { fill(); return via_string(argc) + via_swap(argc) + via_append(argc) + harmless(argc); }
