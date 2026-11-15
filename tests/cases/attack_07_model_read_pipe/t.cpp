// abi.pointer_models gives read (and recv, fread, pread...) the model `none`: "keeps no pointer".
// read writes bytes into its buffer, and those bytes can be a pointer: a task queue in a pipe, the
// self-pipe pattern of event loops. main reads a Task* (and a function pointer) out of a pipe into
// globals that held &idle and fquiet; the entries call through them. The points-to sets contain only
// the initial values.
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
struct Task { virtual long run(long x) = 0; virtual ~Task() = default; };
struct Idle : Task { long run(long x) override { return x; } };
struct Work : Task { long run(long x) override { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 1; } };
static long fquiet(long x) { return x; }
static long floud(long x) { void* p = std::malloc(32); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
Idle idle;
Task* current = &idle;
long (*hook)(long) = fquiet;
__attribute__((noinline)) long entry(long x) { return current->run(x); }
__attribute__((noinline)) long entry_fn(long x) { return hook(x); }
int main(int argc, char**) {
    int fds[2];
    if (pipe(fds) != 0) return 1;
    Task* w = new Work;
    long (*g)(long) = floud;
    if (write(fds[1], &w, sizeof w) != (ssize_t)sizeof w || write(fds[1], &g, sizeof g) != (ssize_t)sizeof g) return 1;
    if (read(fds[0], &current, sizeof current) != (ssize_t)sizeof current) return 1;
    if (read(fds[0], &hook, sizeof hook) != (ssize_t)sizeof hook) return 1;
    long r = entry(argc) + entry_fn(argc);
    std::printf("%ld\n", r);
    delete w;
}
