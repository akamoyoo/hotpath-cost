// thread_local with a non-trivial constructor: per-thread lazy initialisation, __cxa_thread_atexit.
#include "../../fx_test.h"
#include <vector>
struct Counter { std::vector<long> slots; Counter() : slots(64) {} ~Counter() {} };
thread_local Counter tl_counter;
FX_NOINLINE long bump(int i) FX_NB { return ++tl_counter.slots[i & 63]; }
int main(int argc, char**) { long r = 0; for (int i = 0; i < 100; i++) r += bump(i + argc); return (int)(r & 1); }
