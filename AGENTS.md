# Instructions for coding agents

How to use hotpath-cost to answer questions about the cost of a C++ hot path. Read `README.md` for
what the tool found and why it works this way; this file is about doing the work and reporting it
correctly.

## What the tool can answer

| question | command | exact? |
|---|---|---|
| Did this change add an allocation or a lock to a hot operation? | `replay/replay.py LIB --commits BASE,HEAD` then `replay/diff.py LIB` | yes, for the paths the benchmark takes |
| Which line does it come from? | `replay/attribute.py LIB COMMIT` | yes |
| When in the history did it start or stop? | `replay/replay.py LIB --stride N`, then `--refine`, then `diff.py` | yes, among commits that build |
| Which allocating or locking paths does the benchmark never take? | `replay/combine.py LIB COMMIT` | static: "may", not "will" |
| May this function allocate, lock or enter the kernel, on any path? | `static/fn-effects ... --functions` (see `static/README.md`) | depends on the evidence level printed |
| Does the compiler or the optimisation level change the latency? | `replay/matrix.sh 3`, then `replay/matrix_table.py` | only on an idle machine, see below |
| Did a commit change the work done per call (for code that never allocates)? | `replay/replay.py LIB --metric instr`, then `replay/diff.py LIB --instr 5` | yes: instructions per call under callgrind |
| What does a change cost in time? | `replay/price.py LIB A B` | on an idle core, alternating rounds |
| Why is one compiler or level faster? | `replay/gap.py LIB ENTRY` | facts, not proof: read the code |

## Setup

```
./fetch.sh                                  # libraries into src/, full history
(cd static && cmake -S . -B build -G Ninja -DLLVM_DIR=<llvm>/lib/cmake/llvm \
   -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++ && ninja -C build)          # only for static/ and combine.py
```

Environment variables: `CXX_GCC`, `CXX_GCC15`, `CXX_CLANG` (compilers), `REPLAY_OPT` (default `-O2`),
`REPLAY_TAG` (suffix of the output directory), `REPLAY_CORES` (comma-separated cores to pin runs to),
`LLVM_BIN` and `FN_EFFECTS` (for `combine.py`), `VALGRIND` and `VALGRIND_INC` (the valgrind binary and
the directory holding `valgrind/callgrind.h`, for `--metric instr`). Output goes to `out/` (not committed); `replay/export.py`
writes the CSVs in `results/`.

## Recipes

**Check a change for a hot-path regression.**
1. Run `replay/replay.py LIB --commits BASE,HEAD` (commit prefixes) and `replay/diff.py LIB`.
2. If no count changed, report that, with the operations measured. Do not infer anything from the
   `ns` column of these runs.
3. If a count changed, run `replay/attribute.py LIB HEAD` (and `BASE`) and compare the stacks. Name the
   line, the count per call before and after, and whether it is every call or only the first call.
4. Put a price on it only with an alternating timing of both builds on one idle physical core
   (see "Timing").

**Check a change in code that never allocates** (hash maps, parsers, hashing, numerics). Use
`--metric instr` instead of the counts, then `diff.py LIB --instr 5`. An instruction change is real but
its cost is not known: price it with `replay/price.py LIB BASE HEAD` before calling it a regression or
an improvement. In the changes measured so far, time moved in the same direction as instructions by
anything from less than a tenth to twice as much (`docs/insights.md`).

**Explain a compiler gap.** Run `replay/gap.py LIB ENTRY` (ENTRY: a substring of the entry function's
name). Look first at helpers that are out of line in the slow builds only (with their call sites), then
at the number of divisions, then at 256-bit vector instructions. Confirm the cause by changing the
source and timing every compiler with `cases/*/run.sh`-style scripts; a fix for one compiler can slow
another.

**Find which paths the benchmark misses.** Run `replay/combine.py LIB COMMIT` and report the "not
exercised" sites that are not marked cold. Each is a place that may allocate or lock in production and
that the benchmark does not show. Report "static miss" lines as possible defects of the static analysis,
after checking them as described under "Matching".

**Find where a cost entered the history.** Sample with `--stride` (about 50 to 70 samples), then run
`--refine` until it stops, then `diff.py`. A change reported "from X" where X is not the previous
commit means the commits in between do not build with the driver: report an interval, and use
`git log -S'<the line>'` on the attributed line to name the commit.

**Add a library.** Write `replay/drivers/LIB/main.cpp` (operations as `HP_ENTRY` functions with
opaque arguments, and a `main`) and `replay/bench/LIB.cpp` (calls them through `run_ops`), or one
self-contained `replay/bench/LIB.cpp`. Add an entry to `P` in `replay/replay.py`: the paths whose
commits matter, the include directories, extra sources and flags. Check the latest commit first
(`--commits <head>`), then the whole history. If older commits fail with missing standard types
(`uint64_t`, `std::string`, `std::numeric_limits`), include those headers in the benchmark before the
library's header; older headers relied on transitive includes.

## Reading the results

- **Counts are exact; times are not.** Allocations, frees and locks per call are the same on every
  run of a build. Treat any difference of 0.01 or more per call as real.
- **Timing.** Compare times only between builds run alternately, in several rounds, on one physical
  core of an otherwise idle machine. Hyperthread siblings share a core (on the development machine,
  CPU N and N+20). The load on other cores changed one build's time by 40 %, and per-commit timings
  from parallel replays jumped by 2x between commits whose machine code was identical. Report the
  median and the spread between rounds, and call a difference real only if it repeats in every round.
- **First-call counts** include one-time set-up and depend on which operation the benchmark runs
  first. Say "first call" explicitly when reporting them.
- **Static verdicts.** `may` means some path reaches the primitive, not that it runs. `never` and
  `always` are guarantees only at the evidence levels `static/README.md` lists as guarantees.
- **Matching** (`combine.py`) prints how each run-time site was matched: `line`, `caller` (the static
  site is recorded at the line of an inlined callee's caller), `tail` (the run-time stack lost a frame
  to a tail call), `library` (inside a standard-library function). Before calling something a static
  miss, look at the full stack (`attribute.py LIB COMMIT --cc clang --json`) and the static sites of
  the functions on it.
- **A machine-code difference is not a cost.** Measure before reporting one as a regression.

## Proposing a fix upstream

Check that the patched library behaves the same: identical output on many random inputs for
encoders and parsers, a multi-threaded stress test with exactly-once delivery for queues, and the
project's own tests (under ThreadSanitizer where it applies). Time every compiler the project
supports, because a change that helps one compiler can hurt another (`cases/hffix`). Report the
contended and the uncontended case separately for concurrent code. Look for existing issues and pull
requests on the same subject first.

## Known pitfalls

- clang's `mm_malloc.h` declares `posix_memalign` without `noexcept`, which conflicts with
  `replay/counts.h`; a benchmark that includes `<random>` or intrinsics headers hits it with clang.
- quill's benchmark has a backend thread and needs two cores in `REPLAY_CORES`.
- A process may contain only one quill `FrontendOptions` type.
- GCC builds under ThreadSanitizer can be too slow for large tests; the clang build is faster.
- `replay.py` caches results per commit in `out/`: delete that commit's `.jsonl` or `.fail` file after
  changing a benchmark.
