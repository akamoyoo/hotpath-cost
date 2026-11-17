# How the tool works

How each part of the tool works, what did not work, what the tool does not claim, and how to reproduce every number in these documents. Paths are relative to the repository root. See also [AGENTS.md](../AGENTS.md) for task recipes and [static/README.md](../static/README.md) for the static analyser.

## How it works

- `static/` is the LLVM pass plugin and its driver; `static/README.md` describes its rules, evidence
  levels, assumptions and tests.
- `replay/counts.h` replaces `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`, `memalign`,
  `posix_memalign` and `pthread_mutex_lock` in the workload executable (libstdc++'s `operator new`
  and `std::mutex` go through them) and counts per thread. With `COUNTS_TRACE=1` it records the
  return addresses of each event.
- `replay/workload.h` runs each operation once (the first-call counts), warms it up, then counts
  and times it in batches of 100 calls.
- `replay/drivers/<lib>/main.cpp` defines the operations as out-of-line entry functions with opaque
  arguments; `replay/workloads/<lib>.cpp` is the measurement for each library.
- `replay/replay.py` builds the workload against a commit (`git archive`, nothing is installed),
  runs it pinned to a core, and stores one JSON line per operation. `--stride` samples, `--refine`
  bisects. `replay/diff.py` prints the changes; `replay/attribute.py` the stacks;
  `replay/matrix.sh` and `replay/matrix_table.py` the compiler matrix.
- `--metric instr` builds the workload with `-DREPLAY_INSTR` and runs it under valgrind's callgrind.
  For each operation, `workload.h` zeroes callgrind's statistics, runs the operation 20,000 times
  with nothing else in the loop, and dumps the statistics under the operation's name; `replay.py`
  divides. The result is exact: the same build gives the same count on every run. `diff.py --instr PCT`
  reports changes of PCT percent or more, and `--refine` bisects them (`REPLAY_INSTR_PCT`, default 3).
- `replay/price.py` builds two commits and times them alternately for several rounds on one core.
- `replay/gap.py` disassembles one entry function and everything it reaches by direct calls (including
  the parts compilers split off, such as `[clone .cold]`) under each build, and counts instructions,
  call sites, divisions, 256-bit vector instructions and backward jumps, and lists helpers that stay
  out of line in some builds only, with their call sites.
- `replay/combine.py` runs `static/fn-effects` on a library's driver and `attribute.py` on its
  workload at the same commit, both with clang, and matches the sites as described in
  [cases.md](cases.md#static-and-run-time-side-by-side).

## Using it on another library

A library needs two files: `replay/drivers/<lib>/main.cpp` with its hot operations as `HP_ENTRY`
functions, and `replay/workloads/<lib>.cpp`, which calls them through `run_ops` (or one self-contained
workload file, as for spdlog). Then add an entry to `P` in `replay/replay.py` that names the paths
whose commits matter and the include directories. For a change under review, measuring the merge
base and the head is enough: the counts either differ or they do not, and `attribute.py` on the
head names the new line.

## What did not work

- **Comparing machine code alone.** Before counting, the replay compared the machine code of each
  entry function across commits. In atomic_queue it found a sequence of commits (May 2026) whose
  `asm("" :: "r"(ptr))` hints changed GCC's address arithmetic and grew clang's code by 23 %. A
  dependent chain of the three-component `lea` they target is 3 cycles against 1 on this machine,
  and a hand-written copy of GCC's push sequence was 7 % slower with it. The real queue, timed
  across the same commits, showed no difference outside the noise (`cases/atomic_queue/`). A code
  difference is not a cost.
- **Static effect analysis alone.** `fn-effects` (`static/`)
  reports that Liquibook's `callback_now` may allocate both before and after the fix: the fix keeps a
  `reserve` call and makes it a no-op when the capacity suffices. Only run-time counts tell the
  versions apart. In the other direction, the static report at `25b0c5e` has every function of the
  run-time allocation stacks that its (clang) build keeps out of line as "may allocate", and it also
  covers paths the workload never runs, such as an order that trades or a queue that is full. The two answer different questions.
- **Timing every commit.** In the per-commit data, fast_float's parse time halves at one merge and
  doubles again at the next (`e3af1066`, `a7fbcb0a`), and readerwriterqueue alternates between 8 and
  14 ns eleven times. The two fast_float builds have the same machine code instruction for
  instruction, and timed alternately on an idle core they take the same time (14.9 and 14.9 ns).
  The jumps came from the load of the parallel replay. Throughput timings varied by more than
  20 % between identical runs.
  One build of hffix's checksum took 109 ns or 155 ns on the same idle core depending on how busy
  the other cores were; the encoder took up to 55 % longer on a core whose hyperthread sibling was busy. Timings in
  this repository are medians of interleaved rounds on one physical core of an otherwise idle
  machine, and are compared only within one table.

## What is not claimed

- **Counts are for the operations in the drivers, on the paths they take.** An allocation on a path
  the workload does not exercise (an error path, a full queue, a crossing order) is not counted.
- **Allocation and lock primitives outside the list above are not counted**: `mmap`, `futex` used
  directly, spin locks, `new` replaced by the library's own allocator. The first-call counts include
  one-time initialisation of whatever the process does first, so they depend on the order of the
  operations in the workload.
- **"Pinned to a commit" means pinned among the commits that compile with the driver.** When the
  commits next to a change do not compile, the change lies in an interval, as in case 1; the source
  line, not the replay, then names the commit. Bisection finds a change that persists; a change that
  appears and disappears again between two samples is missed.
- **Timings are from one machine** (Xeon E5-2650 v3, Haswell, Ubuntu 24.04, glibc 2.39), single
  threaded, uncontended, with `-march=x86-64-v3`. They show relative cost on this machine, not the
  latency of a production system.
- **No library was found to be wrong.** The changes listed are costs, and upstream removed most of
  them itself, later. Where a library documents a property, the replay checks it only for the
  measured operations.
- **The hffix and MPMCQueue changes are measured proposals, not reviewed patches.** They were checked
  for identical output (hffix) and for exactly-once delivery under a stress test (MPMCQueue), and timed
  on one machine.

## Reproduce

```
./fetch.sh                                   # clone the libraries into src/ (full history)
(cd static && cmake -S . -B build -G Ninja -DLLVM_DIR=<llvm>/lib/cmake/llvm -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++ \
   && ninja -C build && FX_LLVM_BIN=<llvm>/bin python3 tests/run_tests.py)     # the static analyser and its tests
LLVM_BIN=<llvm>/bin CXX_CLANG=<llvm>/bin/clang++ replay/combine.py liquibook 25b0c5ec   # both views
replay/replay.py liquibook                   # every library commit, GCC 13 -O2
replay/diff.py liquibook                     # what changed, and where
replay/attribute.py liquibook 25b0c5ec       # which line allocates
replay/replay.py spdlog --stride 30 && replay/replay.py spdlog --refine && replay/diff.py spdlog
CXX_GCC15=g++-15 CXX_CLANG=clang++-21 replay/matrix.sh 3 && replay/matrix_table.py
CXX_GCC15=g++-15 CXX_CLANG=clang++-21 cases/hffix/run.sh 2
cases/hffix/inline_params.sh 2
CXX_GCC15=g++-15 CXX_CLANG=clang++-21 cases/mpmcqueue/run.sh 2
```

Requirements: Linux x86-64 with glibc, git, Python 3, GCC (13 used here), `taskset`, `addr2line`;
LLVM 21 with CMake and Ninja for the static half and `combine.py`; optionally GCC 15 and Clang 21
for the matrix. `results/` holds the per-commit measurements behind
the tables (`results/<build>/<library>.csv`, `results/matrix.md`), as produced by these commands.
