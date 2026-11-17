# hotpath-cost

**What does the hot path of a C++ library cost: which allocations and locks can it reach, how many does
one call actually perform, at which commit did that change, and how fast is it under each compiler?**

A refactoring, a dependency update or a compiler upgrade can add an allocation or a lock to a hot path,
or make it slower. It passes review and tests, and timing every commit is too noisy to see it. This
repository answers the question in three parts:

- **`static/`: what can happen.** `fn-effects`, an analyser built on LLVM, infers for every function of
  a C++ program, without annotations, whether it may or always allocates, locks, enters the kernel or
  throws, counting everything it calls. It sees every path, but can only say "may".
- **`replay/`: what does happen, commit by commit.** A fixed benchmark is built at every commit of a
  library's history. Allocations and mutex locks per call are counted exactly, so they do not depend
  on machine noise. Changes are bisected to the commit and traced to the source line. Latency is
  compared across GCC 13, GCC 15 and clang 21 at `-O2` and `-O3`.
- **`replay/combine.py`: both views of one commit, side by side.** Static sites are matched against
  run-time stacks. A site can be exercised (with its count per call), reachable but never exercised
  (where the benchmark is blind), or seen at run time but missing from the static result (where the
  analysis is wrong).

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
run-time count tells them apart. In the other direction, the 22 hot sites that this benchmark never
reaches are on paths it does not take, such as stop orders and all-or-none matching.

## What it found

Over 13,246 commits of eighteen open-source low-latency libraries (order book, queues, loggers, FIX,
JSON, formatting, hash maps, number parsing, hashing, linear algebra):

| | finding | |
|---|---|---|
| Liquibook | release 2.0.0 allocates twice as often per order as the code just after it; the fix was never released | [case 1](docs/cases.md#case-1-liquibook-200-allocates-twice-more-per-order) |
| spdlog | a mutex lock removed in 2016 came back in v1.0 to v1.3; a per-thread first-message allocation shipped in eight releases, pinned to the commits that added and removed it | [case 2](docs/cases.md#case-2-spdlog-a-lock-that-came-back-and-a-first-call-allocation-fixed-upstream) |
| hffix | the FIX encoder is 2.8x apart across compilers and levels: GCC `-O2` does not inline the helper that converts constant tags | [case 3](docs/cases.md#case-3-the-same-fix-encoder-28x-apart-between-compilers), [hffix#63](https://github.com/jamesdbrock/hffix/issues/63) |
| MPMCQueue | 1.7x slower with GCC than clang: two 64-bit divisions per operation; a four-line change makes both faster | [case 4](docs/cases.md#case-4-mpmcqueue-a-64-bit-division-that-clang-shortens-and-gcc-repeats), [MPMCQueue#58](https://github.com/rigtorp/MPMCQueue/pull/58) |
| static vs run time | the two views agree on every allocation and lock of the measured Liquibook and concurrentqueue operations | [cases](docs/cases.md#static-and-run-time-side-by-side) |
| fourteen of the eighteen | counts never changed over their measured history (hffix and MPMCQueue among them: their findings are about time) | [results](docs/results.md) |

## Use

```
./fetch.sh                                   # clone the eighteen libraries into src/
replay/replay.py liquibook                   # count at every library commit (GCC 13 -O2)
replay/diff.py liquibook                     # what changed, and at which commit
replay/attribute.py liquibook 25b0c5ec       # which line allocates
replay/combine.py liquibook 25b0c5ec         # static and run-time side by side (needs static/ built)
```

For a change under review, measuring the merge base and the head is enough:
`replay/replay.py LIB --commits BASE,HEAD`. To add a library, write a driver and a benchmark (see
[docs/method.md](docs/method.md#using-it-on-another-library)).

## Documents

- [docs/cases.md](docs/cases.md): the four cases, and the matching of static and run-time sites.
- [docs/results.md](docs/results.md): all eighteen libraries, and the compiler matrix.
- [docs/method.md](docs/method.md): how it works, what did not work, what is not claimed, how to
  reproduce every number.
- [AGENTS.md](AGENTS.md): how a coding agent should run the tool and report results.
- [static/README.md](static/README.md): the static analyser, its rules, evidence levels and tests.

## Limits

- Counts cover the paths the benchmarks take; the static half covers the rest, and can only say "may".
- Only the primitives in `replay/counts.h` are counted (the malloc family and `pthread_mutex_lock`).
- Timings come from one machine (Haswell, Ubuntu 24.04). They are medians of interleaved rounds on an
  idle core, and are compared only within one table.
- No library was found to be wrong. The changes found are costs, and upstream later removed most of
  them itself.

Research quality, Linux x86-64, LLVM 21 for the static half. An independent project, not affiliated with
the LLVM project. Apache-2.0; the libraries are fetched from their own repositories and are not part of
this one.
