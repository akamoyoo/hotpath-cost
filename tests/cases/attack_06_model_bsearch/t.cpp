// abi.pointer_models gives bsearch `ret_arg0` (the result points into argument 0, the key). bsearch
// returns a pointer into argument 1, the array. Here the array is a dispatch table of commands; the
// key is a copy of the default command. The points-to set of c->fn is the key's {quiet}; the call
// runs the table entry's function.
#include <cstdlib>
#include <cstdio>
struct Cmd { long id; long (*fn)(long); };
static long quiet(long x) { return x + 1; }
static long loud(long x) { void* p = std::malloc(64); asm volatile("" : : "r"(p) : "memory"); std::free(p); return x + 2; }
static long other(long x) { return x * 3; }
Cmd table[3] = {{1, other}, {2, loud}, {3, quiet}};
const Cmd default_cmd = {0, quiet};
static int by_id(const void* a, const void* b) { long x = ((const Cmd*)a)->id, y = ((const Cmd*)b)->id; return (x > y) - (x < y); }
__attribute__((noinline)) long entry(long id, long x) {
    Cmd key = default_cmd;
    key.id = id;
    const Cmd* c = (const Cmd*)bsearch(&key, table, 3, sizeof(Cmd), by_id);
    return (c ? c : &key)->fn(x);
}
int main(int argc, char**) { std::printf("%ld\n", entry(argc + 1, argc)); }
