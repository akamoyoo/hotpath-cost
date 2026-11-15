#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>
static long (*f1)(long), (*f2)(long), (*f3)(long);
static void *th(void *p) { return (void *)(f3(3) + f1(1) + f2(2));  /* outer -> inner is this thread's first access to the module's TLS */ }
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { puts(dlerror()); return 1; }
    f1 = dlsym(h, "bump"); f2 = dlsym(h, "bump_local"); f3 = dlsym(h, "outer");
    pthread_t t; pthread_create(&t, 0, th, 0); pthread_join(t, 0);
    return (int)(f3(1) + f1(1) + f2(1)) & 0;
}
