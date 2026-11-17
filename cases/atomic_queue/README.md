# A machine-code difference that costs nothing measurable

The negative result behind "count, don't time" and "a code difference is not a cost" in the README.

**What the code comparison found.** On 2026-05-03 and 2026-05-04, atomic_queue's author tried to steer
instruction selection from the source. `cf9ab816` "Hint compilers to resolve array addresses early" adds
`asm("" :: "r"(elements_))` so that the array base is formed before the index is known. Its comment:
"To avoid resolving a pointer to the array element from this pointer with most expensive 3-arg lea
instruction in the hot path". `559656fd` replaces the trick with `__restrict` parameters.

Three-component `lea` (displacement + base + scaled index) in `AtomicQueue<uint32_t, 1024>::push`
(`asm.sh COMMIT gcc|clang aq_push`, `-O2 -march=x86-64-v3`):

| commit | clang 21 | GCC 13 | GCC 15 |
|---|---|---|---|
| `5e82e9b5` (before) | 0 | 1 | 1 |
| `cf9ab816` (asm hint) | 0 | 0 | 0 |
| `559656fd` (restrict) | 0 | 1 | 1 |
| `b7f1bef` (latest) | 0 | 1 | 1 |

- **GCC.** The hint did what the comment says. The follow-up brought the three-component form back,
  on the path from `lock xadd` to `lock cmpxchg`. The count is the same for every `-march` tried
  (x86-64-v3, haswell, skylake, icelake-server, alderlake, sapphirerapids, znver4).
- **Clang.** It never forms the address in a register here. It folds it into the memory operand of
  `lock cmpxchg`, so the hint had nothing to fix, and it grew `push` from 22 to 27 instructions.

**What the instruction costs in isolation.** A dependent chain of three-component `lea` takes 3
cycles per instruction on this machine (Xeon E5-2650 v3, Haswell), against 1 for the two-component
forms (`lea.c`). LLVM's `llvm-mca` models for haswell and skylake give 1 cycle. A hand-written copy
of GCC's uncontended push sequence (`push.c`) took 43.2 against 40.3 TSC ticks with the
three-component form, 7 % slower.

**What it costs in the real queue.** `bench.cpp` times the real atomic_queue code at the four commits
with both compilers: one thread push+pop, a two-thread round trip, and a one-producer one-consumer
stream (`bench-results.txt`, two rounds, median of seven runs each):

- **Single thread, `AtomicQueue`:** 28.7 to 29.5 ns for every version and both compilers. The latest
  commit, which has the three-component `lea` under GCC, is as fast as `cf9ab816`, which does not.
- **Single thread, `AtomicQueue2`:** GCC 34.0 to 34.2 ns for every version. With clang, `5e82e9b5`
  and the latest commit take 36.4 ns, against 34.1 at the two commits in between, the same in both
  rounds. That is a real 7 % difference, which the `lea` count does not explain (clang has none at
  any of the four commits). It was not investigated further.
- **Round trip, `AtomicQueue`:** within 268 to 284 ns, with no ordering by version.
- **Stream:** varied by more than 20 % between the two rounds of the same build, which hides any
  difference between versions.

The two `lock`-prefixed instructions of a push and a pop take about 28 ns between them on this
machine. Two cycles of address arithmetic between them do not show. The copy in `push.c` overstated
the effect because it measured the sequence alone.
