# Instructions for coding agents

How to use hotpath-cost to answer questions about the cost of a C++ hot path, and how to read what it
prints. `README.md` says what the tool is and what it found; `docs/insights.md` why it works this way.

## What the tool can answer

| question | command | exact? |
|---|---|---|
| Did this change add an allocation, a lock or instructions to a hot operation? | `replay/check.py LIB BASE HEAD` | yes, for the paths the workload takes |
| Do any of the library's open pull requests? | `replay/prs.py LIB` (needs `gh`) | as `check.py` |
| Which line does it come from? | `replay/attribute.py LIB COMMIT` | yes |
| Which stacks did a commit add, remove or change? | `replay/explain.py LIB COMMIT [BASE]` | yes |
| When in the history did it start or stop? | `replay/replay.py LIB --stride N`, then `--refine`, then `diff.py` | yes, among commits that build |
| Which allocating or locking paths does the workload never take? | `replay/combine.py LIB COMMIT` | static: "may", not "will" |
| May this function allocate, lock or enter the kernel, on any path? | `static/fn-effects ... --functions` (see `static/README.md`) | depends on the evidence level printed |
| Does the compiler or the optimisation level change the latency? | `replay/matrix.sh 3`, then `replay/matrix_table.py` | only on an idle machine, see below |
| Did a commit change the work done per call (for code that never allocates)? | `replay/replay.py LIB --metric instr`, then `replay/diff.py LIB --instr 5` | yes: instructions per call under callgrind |
| What does a change cost in time? | `replay/price.py LIB A B` | on an idle core, alternating rounds |
| Why is one compiler or level faster? | `replay/gap.py LIB ENTRY` | facts, not proof: read the code |
| What do the library's own benchmarks allocate and lock per iteration, and when did that change? | `replay/bench.py LIB COMMIT`, `--history`, `--refine`, then `diff.py LIB --tag -bench` | yes, for single-threaded benchmarks |
| What changed in the machine code between two commits? | `replay/gap.py LIB ENTRY --commits A,B` | facts, not proof |

## Where to start

For an open question about a library ("does its hot path have problems?"), in this order:
1. `replay/replay.py LIB --commits <latest library commit>`: what one call costs today, first call and
   steady state.
2. The history: `replay.py LIB --stride N`, `--refine`, `diff.py LIB` (counts), and for code that never
   allocates `--metric instr` and `diff.py LIB --instr 5`. Its last lines give the net change from the
   first measured commit to the latest. A long, active history is where most findings came from.
3. `replay/combine.py LIB COMMIT`: which allocating or locking sites the workload never reaches, if
   the library has a driver and `static/` is built.
4. The compiler matrix and `gap.py`, if the latency differs between compilers or levels.
5. `replay/prs.py LIB`: whether an open pull request changes any of it.

What it costs: a count replay of one library is minutes to an hour (seconds to a minute per commit, in
parallel over `REPLAY_CORES`); an instruction replay runs every workload under callgrind and takes
hours for a long history, so sample it (`--stride`) and refine. `-j` defaults to the number of cores
given, divided by the cores the workload needs.

## Setup

```
./fetch.sh                                  # libraries into src/, full history
(cd static && cmake -S . -B build -G Ninja -DLLVM_DIR=<llvm>/lib/cmake/llvm \
   -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++ && ninja -C build)          # only for static/ and combine.py
```

After changing the tool, run `replay/selftest.py` (about a minute; `--quick` and `--no-static` skip
parts): it checks exact counts and reports the documents rely on, in a temporary output directory.

Measured results already in the repository: `results/gcc/LIB.csv` (counts per commit),
`results/gcc-instr/LIB.csv` (instructions per commit), `results/matrix/` (the compiler matrix).
`replay/export.py` writes them from `out/`.

Environment variables: `CXX_GCC`, `CXX_GCC15`, `CXX_CLANG` (compilers), `REPLAY_OPT` (default `-O2`),
`REPLAY_TAG` (suffix of the output directory), `REPLAY_CORES` (comma-separated cores to pin runs to),
`LLVM_BIN` and `FN_EFFECTS` (for `combine.py`), `VALGRIND` and `VALGRIND_INC` (the valgrind binary and
the directory holding `valgrind/callgrind.h`, for `--metric instr`), `BOOST_INC` (the include directory
of an installed Boost, for Boost libraries). Output goes to `out/` (not committed); `replay/export.py`
writes the CSVs in `results/`.

## Recipes

**Check a change for a hot-path regression.** `replay/check.py LIB BASE HEAD` does steps 1 to 3 in one
run (BASE and HEAD can be any commits of the clone, such as a merge base and a branch head; it exits 1
if a count changed, 2 if only instructions did, 3 if a build or run failed). By hand:
1. Run `replay/replay.py LIB --commits BASE,HEAD` (commit prefixes) and `replay/diff.py LIB`.
2. If no count changed, report that, with the operations measured. Do not infer anything from the
   `ns` column of these runs.
3. If a count changed, run `replay/explain.py LIB HEAD BASE`: the totals per operation, then each
   stack whose count per call changed, with its innermost library frame at both commits. Name the
   line, the count per call before and after, and whether it is every call or only the first call.
   Stacks are matched by function names, so a refactoring that renames functions shows a stack that
   is gone and one that is new; the totals say whether anything changed. For a single commit's stacks,
   use `replay/attribute.py LIB COMMIT`.
4. Put a price on it only with an alternating timing of both builds on one idle physical core
   (see "Timing").

**Check a change in code that never allocates** (hash maps, parsers, hashing, numerics). Use
`--metric instr` instead of the counts, then `diff.py LIB --instr 5`. An instruction change is real but
its cost is not known: price it with `replay/price.py LIB BASE HEAD` before calling it a regression or
an improvement. In the changes measured so far, time moved in the same direction as instructions by
anything from less than a tenth to twice as much (`docs/insights.md`).

**Use the library's own benchmarks instead of a workload.** For a library with Google Benchmark
benchmarks and an entry in `BENCH` in `replay/bench.py` (its CMake options, the benchmark target and the
binary's path), `replay/bench.py LIB COMMIT` builds them at the commit and prints, per benchmark, the
allocations, frees and locks of one iteration and Google Benchmark's time. It needs one Google Benchmark
installed (`GBENCH_DIR`, its install prefix): the build is pointed at it and may not download another,
because versions before 1.8 do not take a fixed iteration count. Each benchmark runs twice, for 2,000
and 12,000 iterations (`--benchmark_min_time=<N>x`), under `LD_PRELOAD=replay/preload/counts_preload.so`;
the difference of the process-wide counts over the difference of the iterations is one iteration, set-up
cancelled. A benchmark whose iteration count is not the one asked for gives no result. Benchmarks that
run several threads (`/threads:N`) are marked inexact and `diff.py` does not compare them.
`--history [--stride N]` and then `--history --refine` replay and bisect as `replay.py` does, into
`out/gcc-bench/LIB/`; `--instr` measures instructions per iteration under callgrind.

**Check a library's open pull requests.** `replay/prs.py LIB` (needs `gh`) fetches every open pull
request that touches the paths in `P` as `refs/hp/pr<N>`, with its target branch, and runs `check.py`
from the merge base. One line per pull request: `no change`, `COUNT CHANGED`, `instructions changed` or
`build or run failed` (the full report follows for the first two). The fetched refs stay in the clone,
so `price.py LIB <base> refs/hp/pr<N>` and `gap.py LIB ENTRY --commits <base>,refs/hp/pr<N>` work on
them afterwards.

**Explain a compiler gap.** Run `replay/gap.py LIB ENTRY` (ENTRY: a substring of the entry function's
name). Look first at helpers that are out of line in the slow builds only (with their call sites), then
at the number of divisions, then at 256-bit vector instructions. Confirm the cause by changing the
source and timing every compiler with `cases/*/run.sh`-style scripts; a fix for one compiler can slow
another. To see what a commit changed in the machine code of one entry, use
`gap.py LIB ENTRY --commits A,B` (one build, two commits): instructions, call sites, divisions, vector
instructions and loops over everything the entry reaches, and the helpers out of line in one only.

**Find which paths the workload misses.** Run `replay/combine.py LIB COMMIT` and report the "not
exercised" sites that are not marked cold. The line "reached k of the n static sites not marked
cold" (per entry, and for all entries at the end) is the coverage to quote; "exercised" counts run-time
stacks, several of which can match one site. Each is a place that may allocate or lock in production and
that the workload does not show. Report "static miss" lines as possible defects of the static analysis,
after checking them as described under "Matching".

**Reach the paths the workload misses.** For a library with an exploration workload
(`replay/workloads/<lib>_explore.cpp`, configured as `<lib>-explore` in `P`, on the same clone through a
link in `src/`), run `replay/combine.py <lib>-explore COMMIT`. The workload replays a seeded random
sequence of realistic operations; the report counts, per entry, how many of the static sites it
exercised. To write one for another library, prepare every action and object before `run_ops` so that
only the library's own allocations are counted, and call the same driver entries as the fixed
workload (add an entry when a path needs a parameter the existing ones fix, such as order conditions).
`combine.py`, `attribute.py` and `explain.py` mark an allocation made by `__cxa_allocate_exception` as an
exception thrown inside the library (`<- AN EXCEPTION ...`), and `combine.py` counts them at the end. In
ordinary use that is a sign of a fault, often one the library catches and hides: look at the full stack
with `attribute.py <lib>-explore COMMIT --cc clang --json`, then reduce the flow to the shortest prefix
that still throws (Liquibook #44 came down to ten orders).

**Find where a cost entered the history.** Sample with `--stride` (about 50 to 70 samples), then run
`--refine` until it stops, then `diff.py`, then `explain.py LIB COMMIT` on each commit it reports
(the base defaults to the commit `diff.py` reports the change "from"). A change printed as an interval, `A..B (N commits in between not measured: ...)`, is
somewhere in those commits, not necessarily at B. If some were not run, run `--refine`; if they fail to
build, look at why (`out/<build>/LIB/<commit>.fail`): a workaround in `P` often makes them build (abseil
included a header GCC rejects for a month). Otherwise report the interval, and use `git log -S'<the
line>'` on the line `explain.py` names to find the commit.

**Add a library.** A useful workload calls each hot operation the way production does, with
realistic sizes (a book with resting orders, a map of 100,000 keys, a message of the usual length);
prepares every input before `run_ops`, so that only the library's own work is counted; keeps any data
structure in a steady state; and puts one operation in each entry function. Write `replay/drivers/LIB/main.cpp` (operations as `HP_ENTRY` functions with
opaque arguments, and a `main`) and `replay/workloads/LIB.cpp` (calls them through `run_ops`), or one
self-contained `replay/workloads/LIB.cpp`. Add an entry to `P` in `replay/replay.py`: the paths whose
commits matter, the include directories, extra sources and flags. Check the latest commit first
(`--commits <head>`), then the whole history. If older commits fail with missing standard types
(`uint64_t`, `std::string`, `std::numeric_limits`), include those headers in the workload before the
library's header; older headers relied on transitive includes. Sources that exist only in some versions
go in `srcs_if_present`, and a library's whole source directory in `srcs_glob` (oneTBB: `src/tbb/*.cpp`); C sources go in `csrcs` or `csrcs_if_present` and are compiled with the C
compiler of the same family (Aeron's C client). A C header that does not declare `extern "C"` itself
is wrapped in the workload, after the headers that switch to C++ headers under C++. Files that the library's own build system would generate go in `gen_files` (QuickFIX's
`Allocator.h`, written by configure); `{tree}` in `flags` is replaced by the commit's source tree, for a
workload that reads the library's data files (QuickFIX's `spec/FIX44.xml`). Libraries to link go in
`libs`, after the sources (Boost.JSON
links Boost.Charconv from the installed release). A library that
needs others from an installed tree (a Boost library and the rest of Boost) uses `sysinc=(DIR,
[patterns])`: the build mirrors DIR with the library's own files (the patterns) left out, so a header
missing at an old commit fails to build instead of being taken from the installed release. When a library's compiled sources reference functions the workload never
reaches and whose definitions pull in much more (abseil's hash-table sampling), name them in
`unresolved_ok`: each gets a stub that traps if called (a run that reaches one fails), while any
other undefined symbol, and every undefined variable, fails the build.

## Reading the results

- **Counts are exact; times are not.** Allocations, frees and locks per call are the same on every
  run of a build. Treat any difference of 0.01 or more per call as real.
- **Timing.** Compare times only between builds run alternately, in several rounds, on one physical
  core of an otherwise idle machine. Hyperthread siblings share a core (on the development machine,
  CPU N and N+20). The load on other cores changed one build's time by 40 %, and per-commit timings
  from parallel replays jumped by 2x between commits whose machine code was identical. Report the
  median and the spread between rounds, and call a difference real only if it repeats in every round.
  Before a price goes to anyone else, confirm it with a second harness: a standalone program that
  calls the operation through one `noinline` function in batches. The replay's workload and such a
  program agreed on every count but disagreed on time by up to 28 points (parallel-hashmap #167:
  insert+erase -11 % against +11 to +17 %), because code layout and the surrounding loop differ.
  Time it on inputs of several sizes and shapes, and with the library's own benchmarks when it has
  them: a change that removes a cost per call and adds one per element can be faster on the input you
  chose and slower on the rest (msgpack-c #1223: 23 % faster on a 4-field message, 40 to 100 % slower
  on arrays and maps).
- **Workloads with another thread** (a logger's backend, an executor's worker; `cores > 1` in `P`):
  frees counted on the calling thread depend on which thread drops the last reference, and vary between
  runs of the same build (Taskflow: 1.4 to 2.8 per call), so `diff.py` and `--refine` do not compare
  them. Locks can vary slightly too (Taskflow `348ba9b4`: 3.99 to 4.00 per call); treat a change smaller
  than one per call there as possible noise and rerun both commits. Instructions per call are not exact there either: the
  calling thread waits for the other, and a build's count varies by about 2 % between runs (Taskflow,
  up to 8 %). `check.py` measures such workloads three times and calls a change only when the ranges do
  not overlap; treat `diff.py --instr` results for them as leads.
- **First-call counts** include one-time set-up and depend on which operation the workload runs
  first. Say "first call" explicitly when reporting them.
- **Static verdicts.** `may` means some path reaches the primitive, not that it runs. `never` and
  `always` are guarantees only at the evidence levels `static/README.md` lists as guarantees.
- **Matching** (`combine.py`) prints how each run-time site was matched: `line`, `caller` (the static
  site is recorded at the line of an inlined callee's caller), `tail` (the run-time stack lost a frame
  to a tail call), `library` (inside a standard-library function), and last `function` (the innermost
  frame is in the function that holds the static site, at another line: an inlined frame came back
  without its name). Before calling something a static
  miss, look at the full stack (`attribute.py LIB COMMIT --cc clang --json`) and the static sites of
  the functions on it.
- **A machine-code difference is not a cost.** Measure before reporting one as a regression.

## Known pitfalls

- clang's `mm_malloc.h` declares `posix_memalign` without `noexcept`, which conflicts with
  `replay/counts.h`; a workload that includes `<random>` or intrinsics headers hits it with clang.
- quill's workload has a backend thread and needs two cores in `REPLAY_CORES`.
- A process may contain only one quill `FrontendOptions` type.
- `replay.py` caches results per commit in `out/`, together with a fingerprint of the workload, its
  driver, the shared headers and the project's entry in `P` (lines that are only a `//` comment do not
  count). When any of them changes, the old results
  are moved to `stale-<fingerprint>/` and the next run measures again. Changes elsewhere (the compiler
  binary itself, an environment variable a workload reads) are not seen: delete the directory then.
- Entry functions must be declared with `HP_ENTRY` (`replay/drivers/hp_entry.h`). An entry that ends in
  a call is compiled as a tail call and disappears from the run-time stack; `attribute.py` and
  `combine.py` build with `-DHP_ENTRY_KEEP_FRAME`, which keeps the frame. Without it, events of that entry
  are not attributed.
- Stacks are recorded 32 frames deep. An event whose entry function is deeper than that is not attributed.
- Exploration workloads should keep the data structure in a steady state (as many removals as
  insertions); one that only grows reports the growth as its steady-state cost.
