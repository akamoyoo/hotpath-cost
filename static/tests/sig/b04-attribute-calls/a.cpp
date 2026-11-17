// Calls that code generation adds because of a function attribute (a build flag), not an instruction.
#define NI __attribute__((noinline))
volatile long sink;
NI long leaf(long x) { return x * 3 + 1; }
NI long hot(long x) { long s = 0; for (long i = 0; i < x + 3; i++) s += leaf(i); return s; }
int main(int argc, char**) { sink = hot(argc > 1 ? 200000 : argc); return 0; }
