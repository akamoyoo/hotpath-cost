// Field-test drivers: every entry is a real out-of-line function (noipa: no inlining into the
// caller, no constant propagation from it, no clones), so its call tree is exactly what the
// project's hot operation compiles to with an opaque receiver and opaque arguments.
// The drivers are built, never executed.
#pragma once
#define FX_ENTRY __attribute__((noipa, used))
