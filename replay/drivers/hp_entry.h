// Entry functions of the drivers: real out-of-line functions (noinline), so that the measured operation is
// what the library's operation compiles to when called from code the compiler cannot see into.
// For call-stack attribution (attribute.py, combine.py build with HP_ENTRY_KEEP_FRAME), an entry also
// keeps its own frame: otherwise a call in tail position becomes a jump, and the entry disappears from
// the stacks it starts. Counting and timing builds are left as they are.
#pragma once
#if defined(HP_ENTRY_KEEP_FRAME) && defined(__clang__)
#define HP_ENTRY __attribute__((noinline, used, disable_tail_calls))
#elif defined(HP_ENTRY_KEEP_FRAME) && defined(__GNUC__)
#define HP_ENTRY __attribute__((noinline, used, optimize("no-optimize-sibling-calls")))
#else
#define HP_ENTRY __attribute__((noinline, used))
#endif
