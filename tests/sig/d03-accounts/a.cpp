// Accounts: own / library / consequence on things a function plainly does itself.
#include <cstdlib>
#include <cstdio>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/mman.h>
#include <condition_variable>
#include <mutex>
#define NI __attribute__((noinline))
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t cv = PTHREAD_COND_INITIALIZER; sem_t sem; volatile int ready;
NI void* shrink(void* p) { return realloc(p, 0); }                       // frees p: is `free` on its own account?
NI void* grow(void* p, long n) { return realloc(p, n); }
NI void wait_ready() { pthread_mutex_lock(&m); while (!ready) pthread_cond_wait(&cv, &m); pthread_mutex_unlock(&m); }   // blocks: syscall?
NI void block_sem() { sem_wait(&sem); }                                   // blocks in futex: syscall account?
NI void unmap(void* p, long n) { munmap(p, n); }
NI void swallow(void (*g)()) { try { g(); } catch (...) { } }            // frees the exception object (__cxa_end_catch)
NI void closef(FILE* f) { fclose(f); }                                    // frees the FILE
NI void bye(int c) { exit(c); }
NI void cxx_wait(std::condition_variable& c, std::unique_lock<std::mutex>& l) { c.wait(l); }
NI void join(pthread_t t) { pthread_join(t, nullptr); }                   // frees/unmaps the thread stack
int main() { return 0; }
