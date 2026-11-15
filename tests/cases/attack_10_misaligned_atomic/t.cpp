// An 8-byte atomic operation on a misaligned object (a member of a packed wire-format struct).
// The IR has `atomicrmw ... align 1` of 8 bytes, within the lock-free width; code generation turns a
// misaligned atomic into a libatomic call (__atomic_fetch_add_8 / __atomic_compare_exchange), and
// libatomic takes a lock for a misaligned object.
#include <cstdio>
#include <cstdint>
struct __attribute__((packed)) Wire { char tag; long seq; };
Wire w;
__attribute__((noinline)) long entry(long x) { return __atomic_fetch_add(&w.seq, x, __ATOMIC_SEQ_CST); }
int main(int argc, char**) { std::printf("%ld %ld\n", entry(argc), entry(argc)); }
