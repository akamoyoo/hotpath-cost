// A library without bitcode (compiled separately, linked as an object/.so): derives from the program's interface.
#include "sink.h"
#include <cstdlib>
static void* volatile keep;
struct LibLoud final : Sink { long on(long x) override { keep = malloc(x + 8); return x; } };
struct LibQuiet final : Sink { long on(long x) override { return x; } };
Sink* make_from_library(int kind) { static LibLoud l; static LibQuiet q; return kind ? (Sink*)&l : (Sink*)&q; }
