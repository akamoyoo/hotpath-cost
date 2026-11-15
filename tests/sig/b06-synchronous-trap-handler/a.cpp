// A handler that runs synchronously, in the same thread, in the middle of f: raise() and __builtin_debugtrap() (int3:
// execution continues after the handler returns). DESIGN assumption 5 puts signal handlers outside the entry's call tree.
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#define NI __attribute__((noinline))
void* volatile keep; volatile long sink;
volatile int in_checkpoint;
static void on_trap(int) { keep = malloc(32); if (in_checkpoint && keep) write(1, "malloc() returned a block inside the handler while checkpoint() was active\n", 75); }
NI long checkpoint(long x) { in_checkpoint = 1; __builtin_debugtrap(); in_checkpoint = 0; return x + 1; }      // never alloc [own]?
NI long notify(long x) { raise(SIGUSR1); return x + 1; }                 // alloc?
int main(int argc, char**) { signal(SIGTRAP, on_trap); signal(SIGUSR1, on_trap); sink = checkpoint(argc) + notify(argc); return 0; }
