// Entry functions of the drivers: real out-of-line functions (noinline), so that the measured operation is
// what the library's operation compiles to when called from code the compiler cannot see into.
#pragma once
#define HP_ENTRY __attribute__((noinline, used))
