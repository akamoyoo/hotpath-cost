#include <cstdlib>
#include <cstdio>
#include <pthread.h>
#include <stdexcept>
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
__attribute__((noinline)) int pure(int x) { return x * 3 + 1; }
__attribute__((noinline)) void* al(int x) { return malloc(x + 16); }
__attribute__((noinline)) void lk() { pthread_mutex_lock(&m); pthread_mutex_unlock(&m); }
__attribute__((noinline)) int thr_catch(int x) { try { if (x) std::__throw_length_error("x"); } catch (...) { return 1; } return 0; }
__attribute__((noinline)) int thr_catch2(int x) { try { std::__throw_length_error("x"); } catch (...) { return 1; } return 0; }
__attribute__((noinline)) void cl(FILE* f) { fclose(f); }
int main(int argc, char**) { void* p = al(argc); lk(); printf("%d %p %d %d\n", pure(argc), p, thr_catch(argc), thr_catch2(argc)); cl(fopen("/dev/null","r")); }
