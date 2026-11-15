// A translation unit built by the project's own build as plain bitcode (clang -emit-llvm, what gllvm/wllvm
// collect), i.e. without -flto/-fwhole-program-vtables: its vtables carry no !type metadata.
#include "sink.h"
#include <cstdlib>
volatile long sink; void* volatile keep2;
struct Quiet final : Sink { void on(long x) override { sink = x; } };
Sink* make_quiet() { static Quiet q; return &q; }
struct LoudSrc final : Src { long get(long x) override { keep2 = malloc(x + 8); return x; } };
Src* make_loud() { static LoudSrc s; return &s; }
