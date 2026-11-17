# hotpath-cost

**An analysis tool for the hot paths of C++ code.** Point it at an operation that has to be cheap
(adding an order, logging a message, parsing a quote, pushing to a queue) and it answers:

- **What can it do?** Every allocation, lock, system call and throw the operation can reach, through
  everything it calls, with the call chain, from a static analysis of the whole program on LLVM IR.
  No annotations.
- **What does it do?** How many allocations and locks one call actually performs, counted exactly,
  and the source line each one comes from.
- **When did that change?** The commit, found by replaying the library's history and bisecting on
  these exact counts, or on the number of instructions a call executes.
- **What does the workload miss?** The paths the static analysis can reach and the run-time view never
  saw, and a random exploration that goes after them.
- **Why is it faster with another compiler?** The machine-code facts behind a gap between GCC and clang
  or `-O2` and `-O3`: helpers left out of line, extra divisions, vector code in one build only.

A refactoring, a dependency update or a compiler upgrade can add an allocation or a lock to a hot path,
or make it slower. It passes review and tests, and timing every commit is too noisy to see it. The
tool works on existing code, unmodified, and every answer it gives names the commit and the line.

## What we learned building it

The tool was applied to 25,567 commits of 38 open-source low-latency and numerical C++ libraries, and
each of these lessons changed how it works ([evidence](docs/insights.md)):

1. **Count, don't time.** Allocations, locks and executed instructions per call are exact and
   repeat on every run; per-commit timings jumped 2x between builds with identical machine code.
   Counts find a change; only an alternating timing of the two commits on an idle core prices it.
2. **Instruction counts find performance changes but do not price them.** In every change timed, time
   moved in the same direction as instructions, by anything from less than a tenth of it to twice as much.
3. **A machine-code difference is not a cost.** Compiler hints that visibly changed a queue's hot
   path cost nothing measurable in the real queue.
4. **A gap between compilers is a lead, with a few usual causes**: a helper left out of line, a
   repeated division, vectorisation in one build only. The fix belongs in the source, and it has to
   be timed with every compiler, because a fix for one compiler can slow another.
5. **The static and the run-time view each see what the other cannot.** Only counts told apart two
   Liquibook versions that may both allocate; only the static view named the 22 allocating paths that
   the workload never took.
6. **The static view tells the workload where to go.** A random order flow written to reach the
   sites the analysis listed raised Liquibook's coverage from 5 to 14 of its 85 hot sites, and turned up a
   memory-corruption bug on the way; random bursts on concurrentqueue showed it freeing and
   reallocating its blocks whenever its length crosses its initial capacity.
7. **Costs enter through refactorings, come back, and stay unreleased.** A lock removed in 2016 came
   back for six releases; an extra allocation per order is in a library's last release; none was
   caught by tests.

## How it is organised

- **`static/`**: `fn-effects`, the LLVM-based analyser: for every function, whether it may or always
  allocates, locks, enters the kernel or throws, and what each answer rests on
  ([static/README.md](static/README.md)).
- **`replay/`**: the run-time half. A small workload per library calls its hot operations through
  out-of-line entry functions. `replay.py` builds it at any commit and counts allocations, frees and
  locks per call (or, with `--metric instr`, instructions per call under callgrind); `diff.py` and
  `--refine` find and bisect changes; `explain.py` and `attribute.py` name the line; `price.py` times two commits
  against each other.
- **`replay/combine.py`**: both views of one commit, matched site by site: exercised, never exercised,
  or missed by the analysis. Exploration workloads (`replay/workloads/<lib>_explore.cpp`) replay a
  seeded random sequence of realistic operations to reach what a fixed workload does not.
- **`replay/check.py`**: a change under review in one command: counts, changed stacks and instructions
  per call at the base and the head.
- **`replay/gap.py`** and **`replay/matrix.sh`**: compiler and level comparisons.
- **`replay/selftest.py`**: checks the tool against results it is known to give.
- **[AGENTS.md](AGENTS.md)**: how a coding agent runs the tool and reads its results.

## Example

Liquibook's order book, adding an order, before and after the commit that fixed an allocation in its
last release (output shortened):

```
$ replay/combine.py liquibook 25b0c5ec          # release 2.0.0's code
== hp_entry::lb_add(...)
   exercised (5):
      alloc src/book/order_book.h:654   OrderBook::add_order      per call 0.50   (line)
      alloc src/book/order_book.h:665   OrderBook::add_order      per call 0.50   (line)
      alloc src/book/depth.h:418        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/depth.h:433        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/order_book.h:1049  OrderBook::callback_now   per call 1.00   (line)
   not exercised: 22 not marked cold, 19 cold
   static miss (0)

$ replay/combine.py liquibook dd5ada83          # the fix, never released
   exercised (4): the same, without callback_now
```

The static list is the same before and after: both versions *may* allocate in `callback_now`. Only the
run-time count tells them apart. In the other direction, the 22 hot sites that this workload never
reaches are on paths it does not take, such as stop orders and all-or-none matching.

## What it found

The cases behind most of the lessons above ([all libraries](docs/results.md)).

**1. Liquibook's last release allocates twice as often per order** ([details](docs/cases.md#case-1-liquibook-200-allocates-twice-more-per-order)).
Allocations per add+cancel, counted at every library commit:

```
25b0c5ec 2017-02-22  add+cancel: alloc 1.00 -> 4.00    order_book.h:1049, a fresh vector on every add and cancel
dd5ada83 2017-02-22  add+cancel: alloc 4.00 -> 2.00    "avoid reallocating callbacks vector"
```

Tag `2.0.0` contains the first commit and not the second, and no later release exists. Price: 433 against
383 ns per add+cancel.

**2. spdlog: a lock that came back, and a first-message allocation** ([details](docs/cases.md#case-2-spdlog-a-lock-that-came-back-and-a-first-call-allocation-fixed-upstream)).
Sampled every 30 of 1,856 commits, then bisected; every change is pinned to a single commit:

```
38456118 2016-12-31  null sink: lock 1 -> 0 per message
0c19bdd7 2018-06-24  null sink: lock 0 -> 1 per message     (a refactoring; v1.0.0 to v1.3.1)
cf152e60 2019-06-27  null sink: lock 1 -> 0 per message
1253a57d 2024-03-29  first message of a thread: alloc 10 -> 11   (MDC thread-local map; v1.14.0 to v1.17.0)
bfa795e5 2026-10-24  first message of a thread: alloc 11 -> 10   (fixed upstream)
```

**3. The same FIX encoder is 2.8x apart between compilers** ([details](docs/cases.md#case-3-the-same-fix-encoder-28x-apart-between-compilers), [hffix#63](https://github.com/jamesdbrock/hffix/issues/63)).
Encoding one order in hffix, ns:

| | GCC 13 | GCC 15 | clang 21 |
|---|---:|---:|---:|
| -O2 | 384 | 301 | 342 |
| -O3 | 178 | 137 | 344 |

At GCC `-O2`, `details::itoa` stays out of line (14 calls per message), so constant tags such as 54 are
converted to ASCII at run time. GCC's `-O3` inlining parameters alone recover 123 of the 207 ns.

**4. MPMCQueue is 1.7x slower with GCC; four lines fix it for every compiler** ([details](docs/cases.md#case-4-mpmcqueue-a-64-bit-division-that-clang-shortens-and-gcc-repeats), [PR #58](https://github.com/rigtorp/MPMCQueue/pull/58)).
`try_push`+`try_pop`, ns:

| | before | after |
|---|---:|---:|
| GCC 13 | 52.5 | 29.6 |
| clang 21 | 30.3 | 26.6 |
| clang 21, slow-division bypass off | 47.7 | |

GCC divides by the capacity twice per operation, because the atomic compare-and-swap forces the
capacity to be reloaded. LLVM hides the cost with its 64-bit division bypass. The change computes the
quotient once.

**5. Eigen 5's 4x4 Cholesky solve is 1.5x slower with GCC** ([details](docs/cases.md#case-5-eigen-5s-4x4-cholesky-solve-is-15x-slower-with-gcc)).
Found by the instruction replay: commit `28c3b26d` "masked load/store framework" (April 2025) raised
`A.llt().solve(b)` for a `Matrix4d` from 548 to 732 instructions per call. Timed alternately: 1.49x
slower with GCC 13, 1.40x with GCC 15, 1.11x with clang; between releases 3.4.1 and 5.0.1, 1.48x with
GCC. `gap.py` shows a new out-of-line masked-segment loop for the factorisation's 1-to-3-element
updates. Not yet reported upstream.

**6. A random order flow reaches the hidden paths, and finds an out-of-bounds write in Liquibook**
([details](docs/cases.md#case-6-a-random-order-flow-reaches-the-paths-and-finds-an-out-of-bounds-write-in-liquibook)).
Over Liquibook's five entry functions the static analysis lists 85 allocation sites not marked cold; the
fixed workload reaches 5 of them and a seeded random order flow 14. One of them was `__cxa_allocate_exception`: the
book threw "Unexpected ignore_ask_fill_qty_" in ordinary order flow. Reduced to ten orders, it came
down to `Depth::find_level` returning a past-the-end pointer instead of `nullptr`, so fills of a
triggered stop order overwrite the members after the price levels
([liquibook#44](https://github.com/enewhuis/liquibook/issues/44), fix and tests in
[#45](https://github.com/enewhuis/liquibook/pull/45)).

**7. msgpack-c allocates twice per message when unpacking into a reused zone**
([details](docs/cases.md#case-8-msgpack-c-allocates-twice-per-message-when-unpacking-into-a-reused-zone)).
Reusing a `msgpack::zone` is how a caller keeps unpacking off the heap, but each `unpack()` still reserves
two `std::vector` parse stacks: 2 allocations per message, 1 before the visitor API of 2016 (`d15d9220`).
Keeping their first 32 levels inline removes both: 118.6 -> 83.7 ns per message with GCC 13, 119.1 -> 98.3
with clang 21 ([msgpack-c#1222](https://github.com/msgpack/msgpack-c/issues/1222), fix and test in
[#1223](https://github.com/msgpack/msgpack-c/pull/1223)).

**8. Open pull requests, checked.** `check.py` on the 148 open pull requests of 14 of these libraries
that touch their hot paths: 122 change nothing, and among the rest, a pull request titled "AVX2
optimization of group lookup" makes parallel-hashmap's `find` 13 % slower in this workload and
insert+erase 11 % faster, and one titled "Improving speed ..." makes fast_float's parsing of `100` 19 %
slower ([details](docs/results.md#open-pull-requests)).

**Static and run time agree.** On every measured operation of Liquibook and concurrentqueue, every
allocation and lock seen at run time matches a site of the static analysis (no static miss), and the
static analysis lists the paths the workloads never take ([details](docs/cases.md#static-and-run-time-side-by-side)).

## Use

```
./fetch.sh                                         # clone the libraries into src/
replay/replay.py liquibook                         # allocations and locks at every library commit
replay/diff.py liquibook                           # what changed, and at which commit
replay/explain.py liquibook 25b0c5ec               # which stacks that commit added, removed or changed
replay/attribute.py liquibook 25b0c5ec             # every allocating and locking stack at one commit
replay/combine.py liquibook 25b0c5ec               # static and run-time side by side (needs static/ built)
replay/replay.py fast_float --metric instr         # instructions per call at every commit (valgrind)
replay/diff.py fast_float --instr 5                # commits that changed them by 5 % or more
replay/price.py fast_float 254f10ce 3044c9b1       # what such a change costs in time
replay/gap.py hffix fix_write_new_order            # why one compiler is faster than another
replay/selftest.py                                 # check the tool against results it is known to give
```

For a change under review, `replay/check.py LIB BASE HEAD` measures the merge base and the head:
counts, the stacks that changed, and instructions per call. To add a library, write a driver and a workload (see
[docs/method.md](docs/method.md#using-it-on-another-library)).

## Documents

- [docs/insights.md](docs/insights.md): the lessons above, with their evidence.
- [docs/cases.md](docs/cases.md): the cases, and the matching of static and run-time sites.
- [docs/results.md](docs/results.md): all libraries, and the compiler matrix.
- [docs/method.md](docs/method.md): how it works, what did not work, what is not claimed, how to
  reproduce every number.
- [AGENTS.md](AGENTS.md): how a coding agent should run the tool and report results.
- [static/README.md](static/README.md): the static analyser, its rules, evidence levels and tests.

## Limits

- Counts cover the paths the workloads take; the static half covers the rest, and can only say "may".
- Only the primitives in `replay/counts.h` are counted (the malloc family and `pthread_mutex_lock`).
- Timings come from one machine (Haswell, Ubuntu 24.04). They are medians of interleaved rounds on an
  idle core, and are compared only within one table.
- Apart from Liquibook's out-of-bounds write, the changes found are costs, not bugs, and upstream later
  removed most of them itself.

Research quality, Linux x86-64, LLVM 21 for the static half. An independent project, not affiliated with
the LLVM project. Apache-2.0; the libraries are fetched from their own repositories and are not part of
this one.
