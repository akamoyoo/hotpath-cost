// Shared by the test programs. FX_NB marks an entry as [[clang::nonblocking]] for the RealtimeSanitizer
// build (-DHP_RTSAN); the static analysis names entries on the command line and does not need it.
#pragma once
#ifdef FX_RTSAN
#define FX_NB [[clang::nonblocking]]
#else
#define FX_NB
#endif
#define FX_NOINLINE __attribute__((noinline))
