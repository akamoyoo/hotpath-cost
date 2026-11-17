#include <vector>
using Fn = int (*)(int);
int alloc_ctor(int x);
Fn g_cb = nullptr;
struct Registrar { Registrar() { g_cb = &alloc_ctor; } } g_registrar;
