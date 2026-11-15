/* another object of the same link (not in the analysed module): strong definitions for the weak symbols */
#include <stdlib.h>
static volatile long s; static void *volatile k;
void hook(long x) { s = x; }
void hookq(long x) { k = malloc(x + 8); }
void weakdef(long x) { s = x; }
