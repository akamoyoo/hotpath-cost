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
JSON, formatting, hash maps, number parsing, hashing, linear algebra). Fourteen never changed their
allocation and lock counts over their measured history ([all results](docs/results.md)). The four
cases below are the ones that did change, or whose latency depends on the compiler.

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

**Static and run time agree.** On every measured operation of Liquibook and concurrentqueue, every
allocation and lock seen at run time matches a site of the static analysis (no static miss), and the
static analysis lists the paths the benchmarks never take ([details](docs/cases.md#static-and-run-time-side-by-side)).

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
