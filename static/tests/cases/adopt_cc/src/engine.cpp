// A small "existing project": built with GCC flags, one file (legacy.c) uses a GCC-only extension.
extern "C" int legacy_checksum(const unsigned char* p, int n);   // in legacy.c
#include <vector>
static std::vector<int> g_orders;
__attribute__((noinline)) int on_tick(int px) { return px * 2 + 1; }
__attribute__((noinline)) int on_order(int id) { g_orders.push_back(id); return (int)g_orders.size(); }
__attribute__((noinline)) int on_packet(const unsigned char* p, int n) { return legacy_checksum(p, n) + on_tick(n); }
