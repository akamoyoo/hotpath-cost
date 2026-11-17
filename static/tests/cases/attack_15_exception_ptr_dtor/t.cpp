// An exception created outside the entry (std::make_exception_ptr in main) is rethrown and caught
// inside the entry. When the catch block ends, the last reference goes and __cxa_end_catch ->
// _Unwind_DeleteException -> __gxx_exception_cleanup runs the exception object's destructor. The
// destructor's address was handed to the library in main; inside the entry no call passes it, and the
// summaries of the calls made there report no unresolved indirect call (the binary analyser's
// `personality` model resolves the cleanup call in _Unwind_DeleteException to the personality routines).
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <utility>
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
struct Err {
    long v;
    ~Err() {
        void* p = std::malloc(100); asm volatile("" : : "r"(p) : "memory"); std::free(p);
        pthread_rwlock_wrlock(&rw); pthread_rwlock_unlock(&rw);
        (void)!write(2, "", 0);
    }
};
std::exception_ptr pending;
__attribute__((noinline)) long entry() {
    std::exception_ptr e = std::move(pending);
    try { std::rethrow_exception(e); } catch (const Err& x) { return x.v; }
    return 0;
}
int main(int argc, char**) {
    pending = std::make_exception_ptr(Err{argc});
    std::printf("%ld\n", entry());
}
