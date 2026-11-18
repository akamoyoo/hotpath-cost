# hotpath-cost

**An analysis tool for the hot paths of C++ code.** Point it at an operation that has to be cheap
(adding an order, logging a message, parsing a quote, pushing to a queue) and it tells you what one call
costs, counted exactly (allocations, locks, instructions), where each cost comes from, which commit
changed it, which paths your tests never take, and why one compiler is faster than another. It works
on existing code, unmodified, and every answer names the commit and the line.

A refactoring, a dependency update or a compiler upgrade can add an allocation or a lock to a hot path,
or make it slower. It passes review and tests, and timing every commit is too noisy to see it. Exact
counts are not.

## Problems it found in real projects

Applied to 38 open-source C++ libraries used in trading, messaging and numerics (25,567 library
commits replayed, 148 open pull requests checked). What we reported upstream, with a fix where one
was ours to write:

### Accepted upstream

| library | what the tool found | found by | our change |
|---|---|---|---|
| [MPMCQueue](https://github.com/rigtorp/MPMCQueue) (lock-free queue) | **1.7x slower with GCC than with clang**: a 64-bit division by the capacity, done twice per operation | the compiler matrix, then machine-code facts | [issue #57](https://github.com/rigtorp/MPMCQueue/issues/57) and a four-line fix, merged by the author ([PR #58](https://github.com/rigtorp/MPMCQueue/pull/58)): 52.5 -> 29.6 ns with GCC, faster with clang too |

### Reported upstream, under review

| library | what the tool found | found by | our report |
|---|---|---|---|
| [Liquibook](https://github.com/enewhuis/liquibook) (order book) | **An out-of-bounds write.** `Depth::find_level` returns a past-the-end pointer instead of `nullptr`, so fills of a triggered stop order overwrite the book's members; in ordinary order flow | a random order flow aimed at the paths the static analysis listed, which surfaced an unexpected exception | [issue #44](https://github.com/enewhuis/liquibook/issues/44); fix with regression tests in [PR #45](https://github.com/enewhuis/liquibook/pull/45) |
| [msgpack-c](https://github.com/msgpack/msgpack-c) (MessagePack) | **2 heap allocations per message** when unpacking into a reused zone, the documented way to avoid them; since the visitor API of 2016 | exact counts, then the allocating stacks | [issue #1222](https://github.com/msgpack/msgpack-c/issues/1222); fix in [PR #1223](https://github.com/msgpack/msgpack-c/pull/1223), CI green: 0 allocations, 118.6 -> 83.7 ns |
| [hffix](https://github.com/jamesdbrock/hffix) (FIX protocol) | **The same encoder 2.8x apart** between compilers and levels: an integer-to-ASCII helper left out of line converts constant tags at run time | the compiler matrix, then machine-code facts | [issue #63](https://github.com/jamesdbrock/hffix/issues/63); fix in [PR #64](https://github.com/jamesdbrock/hffix/pull/64) |
| [Eigen](https://gitlab.com/libeigen/eigen) (linear algebra) | **Eigen 5's 4x4 Cholesky solve 1.48x slower than 3.4** with GCC; the step is one commit of April 2025 | instructions per call across the history, then timing | the bisection and timings, added to the maintainers' open [issue #3206](https://gitlab.com/libeigen/eigen/-/issues/3206) |
| [parallel-hashmap](https://github.com/greg7mdp/parallel-hashmap) (hash map) | **An open "AVX2 optimization" that is slower** in every case measured, up to 42 % for missing keys in a large table | checking every open pull request, then a standalone benchmark | the benchmark, posted on [PR #167](https://github.com/greg7mdp/parallel-hashmap/pull/167#issuecomment-6961404564), whose open question was whether it helps |

The issues, pull requests and comments above were written and submitted by us (GitHub
[akamoyoo](https://github.com/akamoyoo)), each with the numbers behind it; their state is as of
November 17, 2026.

### Found in the history, already fixed upstream or by design

| library | what the tool found | found by |
|---|---|---|
| [Liquibook](https://github.com/enewhuis/liquibook) | **Its last release allocates twice as often per order** (1 -> 4 per add+cancel, 433 against 383 ns); fixed the next day in a commit no release contains, and there has been no release since 2017 | the history replay |
| [spdlog](https://github.com/gabime/spdlog) (logger) | **A mutex lock per message** on the null sink came back in a 2018 refactoring for six releases; **an allocation and a TLS destructor on every thread's first message** for eight releases (2024-2026); both since fixed by the maintainers | the history replay, bisected to single commits |
| [abseil](https://github.com/abseil/abseil-cpp) (`flat_hash_map`) | **Refactorings that cost and fixes that follow:** one made `operator[]` 50 % more instructions and 37 % slower, undone by the maintainers ten days later; insert+erase is 13 to 16 % slower than in 2018 | instructions per call across the history, then timing |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue) | **A malloc and a free per 32 elements** whenever the queue's length crosses its initial capacity, since 2022; deliberate, documented, switched off by a trait, and close to free on one thread | a random enqueue/dequeue flow aimed at the sites the static analysis listed |

Details and how each was found: [docs/cases.md](docs/cases.md). All libraries, including the quiet
ones and the other open pull requests: [docs/results.md](docs/results.md).

## How it works

Two views of the same code, matched site by site:

- **Static** ([`static/`](static/README.md), `fn-effects`, an LLVM 21 pass): for every function of the
  program, whether it may or always allocates, locks, enters the kernel or throws, through everything
  it calls, with the call chain and the evidence each answer rests on. No annotations.
- **Run time** (`replay/`): a small workload per library calls its hot operations through out-of-line
  entry functions. The tool builds it at any commit and counts allocations, frees and mutex locks per
  call exactly (or instructions per call, under callgrind), records the stack of each, and replays the
  library's history to find and bisect every change.

`combine.py` puts the two side by side: what the workload exercised, what the static analysis says is
reachable but the workload never reached, and anything seen at run time that the analysis missed
(none, on Liquibook and concurrentqueue, where the two were matched). Liquibook's order book, release 2.0.0's code against the fix:

```
$ replay/combine.py liquibook 25b0c5ec
== hp_entry::lb_add(...)
   reached 4 of the 19 static sites not marked cold
   exercised (5):
      alloc src/book/order_book.h:654   OrderBook::add_order      per call 0.50   (line)
      alloc src/book/order_book.h:665   OrderBook::add_order      per call 0.50   (line)
      alloc src/book/depth.h:418        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/depth.h:433        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/order_book.h:1049  OrderBook::callback_now   per call 1.00   (line)
   static miss (0)

$ replay/combine.py liquibook dd5ada83          # the fix
   exercised (4): the same, without callback_now
```

Both versions *may* allocate in `callback_now`; only the run-time count tells them apart. The sites the
workload never reaches (stop orders, all-or-none matching) are where the random order flow went next,
and where it found the out-of-bounds write.

## Use

```
./fetch.sh                                     # clone the libraries into src/
replay/check.py LIB BASE HEAD                  # a change under review: counts, changed stacks, instructions
replay/prs.py LIB                              # the same for every open pull request (needs gh)
replay/replay.py LIB [--metric instr]          # counts (or instructions) at every library commit
replay/diff.py LIB [--instr 5]                 # which commits changed them
replay/explain.py LIB COMMIT                   # which stacks that commit added, removed or changed
replay/combine.py LIB COMMIT                   # static and run time side by side (needs static/ built)
replay/price.py LIB A B                        # what a change costs in time, alternating on one core
replay/gap.py LIB ENTRY                        # why one compiler or level is faster
replay/selftest.py                             # check the tool against results it is known to give
```

To add a library, write a workload (and, for the static view, a driver): see
[docs/method.md](docs/method.md#using-it-on-another-library). [AGENTS.md](AGENTS.md) is written for a
coding agent running the tool: every command, how to read its output, the known pitfalls.

## What we learned building it

Each of these changed how the tool works ([evidence](docs/insights.md)):

1. **Count, don't time.** Per-commit timings jumped 2x between builds with identical machine code.
   Allocations, locks and instructions per call are exact; counts find a change, and only an
   alternating timing on an idle core, confirmed by a second harness, prices it.
2. **Instruction counts find performance changes but do not price them.** Time moved in the same
   direction in every change timed, by anything from a tenth of the instruction change to twice it.
3. **A machine-code difference is not a cost**, and an allocation is not always one either: a malloc
   and a free per 32 elements cost concurrentqueue 0.5 % on one thread.
4. **A gap between compilers is a lead, with a few usual causes**: a helper left out of line, a
   repeated division, vectorisation in one build only. The fix belongs in the source and has to be
   timed with every compiler.
5. **The static and the run-time view each see what the other cannot**, and **the static view tells a
   workload where to go**: aimed at the sites the analysis listed, a random order flow raised
   Liquibook's coverage from 5 to 14 of 85 hot sites and found a memory-corruption bug.
6. **Costs enter through refactorings, come back, and stay unreleased.** None of the cases above was
   caught by the library's tests.

## Repository

- `static/`: the static analyser, its rules, evidence levels and 1,180 tests ([README](static/README.md)).
- `replay/`: the run-time tools above; `replay/workloads/` holds one workload per library,
  `replay/drivers/` the entry functions the static view analyses.
- `cases/`: scripts and outputs that reproduce the compiler cases (hffix, MPMCQueue, atomic_queue,
  Eigen) and the attributions of Liquibook and spdlog.
- `results/`: per-commit measurements behind every table (`results/gcc/`, `results/gcc-instr/`), and
  the compiler matrix (`results/matrix/`).
- `docs/`: [cases](docs/cases.md), [all libraries](docs/results.md), [lessons](docs/insights.md),
  [method, what did not work and how to reproduce](docs/method.md).

## Limits

- Counts cover the paths the workloads take; the static half covers the rest, and can only say "may".
- Only the malloc family and `pthread_mutex_lock` are counted (`replay/counts.h`).
- Instruction counts of workloads with a second thread vary between runs; they are compared as ranges.
- Timings come from one machine (Haswell, Ubuntu 24.04) and are compared only within one table.

Research quality, Linux x86-64, LLVM 21 for the static half. An independent project, not affiliated with
the LLVM project. Apache-2.0; the libraries are fetched from their own repositories and are not part of
this one.
