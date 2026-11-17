// The other translation unit: small helpers the pre-link pipeline of a.cpp cannot see into, the LTO link can.
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <string>
#include <pthread.h>
#include <unistd.h>
void* make(long n) { return malloc(n); }
void release(void* p) { free(p); }
void* make_zero(long n) { return calloc(1, n); }
void* make_aligned(long n) { return aligned_alloc(64, n); }
void* grow(void* p, long n) { return realloc(p, n); }
long* make_obj(long v) { return new long(v); }
void drop_obj(long* p) { delete p; }
long* make_arr(long n) { return new long[4]; }
void drop_arr(long* p) { delete[] p; }
void* nothing() { return nullptr; }
long* nothing_obj() { return nullptr; }
long sum3(long a) { std::vector<long> v{a, a + 1, a + 2}; return v[0] + v[1] + v[2]; }
long len_of(const char* s) { std::string t(s); return (long)t.size(); }
void* make_nt(long v) { return new (std::nothrow) long(v); }
int thrower(long x) { throw x; }
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
void lock_unlock() { pthread_mutex_lock(&mu); pthread_mutex_unlock(&mu); }
long wr0(int fd) { return write(fd, "", 0); }
