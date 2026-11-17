# hotpath-cost

**What does the hot path of a C++ library cost: which allocations and locks can it reach, how many does
one call actually perform, at which commit did that change, and how fast is it under each compiler?**

Two halves answer the two kinds of question, and a third part puts them side by side:

- **`static/`: what can happen.** `fn-effects`, an analyser built on LLVM, reads the whole program as
  bitcode and infers for every function, without annotations, whether it may or always allocates,
  locks, enters the kernel or throws, counting everything it calls (`static/README.md`). It sees every
  path, but it can only say "may".
- **`replay/`: what does happen, commit by commit.** A fixed benchmark is built at every commit of a
  library's history. Heap allocations, frees and mutex locks are counted exactly per call; changes are
  bisected to the commit and traced to the source line; latency is compared between GCC 13, GCC 15 and
  clang 21 at `-O2` and `-O3`. It is exact, but it sees only the paths the benchmark takes.
- **`replay/combine.py`: both at once.** For each entry function of one commit, the static sites are
  matched against the run-time stacks: sites that are exercised (with their count per call), sites that
  are reachable but never exercised (where the benchmark is blind), and run-time events the static
  analysis missed (where the analysis is wrong).

Three ideas make it work on real libraries and their whole history:

- **Count, don't time.** Allocations, frees and mutex acquisitions per call are exact. For a given
  build they are the same on every run and under any load. A change of one allocation per call is
  visible at every commit, while the change in latency it causes (tens of nanoseconds) is below the
  run-to-run noise of a timing benchmark. Time is measured too, but only to put a price on a change
  once the counts have found it.
- **Bisect on exact counts.** Because the counts are deterministic, a history can be sampled every N
  commits and every interval whose endpoints differ can be bisected down to a single commit. The commit
  is then built again with call-stack recording on, and each allocation or lock is attributed to a
  source line, inlined frames included.
- **Check one view against the other.** A run-time event the static analysis does not list is a bug
  in the analysis; a static site the benchmark never reaches is a gap in the benchmark.

In short, over 13,246 commits of eighteen libraries:

- Liquibook's last release allocates twice as often per order as the code just after it (case 1).
- spdlog re-added a mutex lock that had been removed, for six releases. A first-message allocation it
  shipped for eight releases was fixed upstream in October 2026; the replay pins both to the commits
  that made them (case 2).
- The same FIX encoder (hffix) is 2.8x apart between compilers and levels, because GCC at `-O2` does
  not inline the helper that turns constant tags into ASCII (case 3). Reported upstream
  ([hffix#63](https://github.com/jamesdbrock/hffix/issues/63)).
- A lock-free queue (MPMCQueue) is 1.7x slower with GCC than with clang because GCC divides twice
  per operation in 64 bits. A four-line change makes both compilers faster (case 4;
  [MPMCQueue#58](https://github.com/rigtorp/MPMCQueue/pull/58)).
- The static and the run-time view agree on every allocation and lock of the measured Liquibook and
  concurrentqueue operations, and the static view names the paths the benchmarks never take, such as
  Liquibook's matching of all-or-none orders.
- Fourteen of the eighteen libraries never changed their counts over their measured history, and
  that is a result too.

`AGENTS.md` tells a coding agent how to run the tool on a change or a library and how to report the
results. Status: research quality, Linux x86-64, LLVM 21 for the static half. Read "What is not claimed" (here
and in `static/README.md`) before quoting a number. An independent project, not affiliated with the
LLVM project.

## What was found

Eighteen libraries, 13,246 library commits in their first-parent histories, 2,050 of them measured.
Thirteen libraries were measured at every library commit where the driver compiles. quill was sampled
every fifth commit, spdlog every 30th, fmt every 50th, simdjson every 4th and Eigen every 100th, and
every interval whose counts differed was bisected. Built with GCC 13.3, `-O2 -march=x86-64-v3`. Per-commit data:
`results/gcc/<library>.csv`.

| library | measured | count changes | what changed |
|---|---|---|---|
| [Liquibook](https://github.com/enewhuis/liquibook) (order book) | 13 of 60 | 2 | +2 allocations per add+cancel in release 2.0.0; fixed after it, never released (case 1) |
| [spdlog](https://github.com/gabime/spdlog) (logger) | 80 of 1,856, bisected | 6 | a mutex lock per message on `null_sink_mt` in v1.0.0 to v1.3.1; an allocation and a TLS destructor on each thread's first message in v1.14.0 to v1.17.0, fixed upstream in October 2026 (case 2) |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue) | 118 of 136 | 1 | the first `try_enqueue` of each thread takes a mutex since a 2022 use-after-free fix (`3e72a15`); it also allocates 3 times, before and after |
| [simdjson](https://github.com/simdjson/simdjson) | 53 of 272, bisected | 3 | first parse with a new parser: 6, then 10, then 11, then 6 allocations again (buffers); 0 per parse after it throughout |
| [quill](https://github.com/odygrd/quill) (logger) | 125 of 333 | 0 | `LOG_INFO`: 0 allocations, 0 locks, first call included |
| [fmt](https://github.com/fmtlib/fmt) | 61 of 3,407 | 0 | `format_to(memory_buffer)`: 0; `fmt::format` of a 41-character string: 1 (the `std::string` itself) |
| [atomic_queue](https://github.com/max0x7ba/atomic_queue) | 116 of 119 | 0 | 0 and 0 (`3ae8dff` crashes the benchmark) |
| [SPSCQueue](https://github.com/rigtorp/SPSCQueue) | 22 of 22 | 0 | 0 and 0 |
| [MPMCQueue](https://github.com/rigtorp/MPMCQueue) | 7 of 7 | 0 | 0 and 0; but GCC's build is 1.7x slower than clang's on the uncontended path, fixed by a four-line change (case 4) |
| [hffix](https://github.com/jamesdbrock/hffix) (FIX) | 15 of 69 | 0 | 0 and 0; but the encoder is 2.8x apart between compilers and levels (case 3) |
| [readerwriterqueue](https://github.com/cameron314/readerwriterqueue) (SPSC queue) | 52 of 80 | 0 | 0 and 0 |
| [unordered_dense](https://github.com/martinus/unordered_dense) (hash map) | 285 of 323 | 0 | find, insert+erase, `operator[]`: 0 and 0 |
| [robin-map](https://github.com/Tessil/robin-map) (hash map) | 54 of 55 | 0 | 0 and 0 |
| [parallel-hashmap](https://github.com/greg7mdp/parallel-hashmap) (hash map) | 287 of 316 | 0 | 0 and 0 |
| [robin-hood-hashing](https://github.com/martinus/robin-hood-hashing) (hash map) | 221 of 224 | 0 | 0 and 0 |
| [fast_float](https://github.com/fastfloat/fast_float) (number parsing) | 212 of 213 | 0 | 0 and 0 |
| [xxHash](https://github.com/Cyan4973/xxHash) (hashing) | 290 of 346 | 0 | 0 and 0 |
| [Eigen](https://gitlab.com/libeigen/eigen) (linear algebra, fixed sizes) | 39 of 5,408, sampled | 0 | 0 and 0 |

"Measured" counts the commits at which the fixed benchmark driver compiles and runs; older versions
with a different API are not measured. The quiet rows are results too: these libraries keep their
hot path at zero allocations and locks, and over every measured commit nothing slipped in. For the
numerical and hashing libraries the counts are zero by design; what differs there is time, which the
compiler matrix below compares.

## Case 1: Liquibook 2.0.0 allocates twice more per order

Liquibook's order book, adding an order at a price outside a resting book of 4,000 orders and
cancelling it again:

```
$ replay/replay.py liquibook && replay/diff.py liquibook
25b0c5ec 2017-02-22 TransId is an idea who's time never came.  Eliminate it.   (from 492c842c)
    add+cancel: alloc 1.00 -> 4.00; free 1.00 -> 4.00; first_alloc 1 -> 4
dd5ada83 2017-02-22 avoid reallocating callbacks vector.   (from 25b0c5ec)
    add+cancel: alloc 4.00 -> 2.00; free 4.00 -> 2.00; first_alloc 4 -> 2
```

The three library commits between `492c842` and `25b0c5e` do not compile with GCC 13. The attribution
at `25b0c5e` (`cases/liquibook/attribution-25b0c5e.txt`, signatures shortened here) names the line:

```
  allocation 1.000 per call
      liquibook::book::OrderBook<SimpleOrder*>::callback_now()  src/book/order_book.h:1049
      liquibook::book::OrderBook<SimpleOrder*>::add(SimpleOrder* const&, unsigned int)  src/book/order_book.h:429
  allocation 1.000 per call
      liquibook::book::OrderBook<SimpleOrder*>::callback_now()  src/book/order_book.h:1049
      hp_entry::lb_cancel(...)
```

Line 1049 is `working.reserve(callbacks_.capacity())` on a fresh local vector, so every add and
every cancel allocates. `git log -S'working.reserve'` shows the line came in with `38bcc84` "Simplify
Callback object" (2017-02-21), one of the commits that do not compile, and left with `dd5ada8` the
next day. The release tag `2.0.0` contains `38bcc84` and not `dd5ada8`, and no later tag exists: the
last release of Liquibook carries the extra allocations. The two remaining allocations per add+cancel
are the order's node in the price-level map and a new depth level (`order_book.h:654/665`,
`depth.h:418/433`).

Price, timed alternately on one core, median of five runs: 433 ns per add+cancel at `25b0c5e`
against 383 ns at `dd5ada8`, about 25 ns per allocation with glibc's allocator on one thread.

## Case 2: spdlog, a lock that came back, and a first-call allocation fixed upstream

A synchronous `spdlog::logger` writing to `null_sink_mt` (also a common way to switch logging off),
sampled every 30 commits and bisected:

```
38456118 2016-12-31 Update null_sink.h                                   null sink info: lock 1.00 -> 0.00
0c19bdd7 2018-06-24 pattern per sink and pattern caching                 null sink info: lock 0.00 -> 1.00
cf152e60 2019-06-27 Merge branch 'v1.x' ...                              null sink info: lock 1.00 -> 0.00
```

Every change is pinned to one commit (the commits on both sides are adjacent in the first-parent
history). The 2018 refactoring changed one line in `sinks/null_sink.h`, `using null_sink_mt =
null_sink<std::mutex>`, back to what a 2016 commit had removed; a one-line commit (`a680b71d`,
merged in `cf152e60`) removed it again a year later. Releases v1.0.0 to v1.3.1 lock a mutex on every
call to a logger that discards its output (`cases/spdlog/attribution-0c19bdd7.txt`:
`base_sink<std::mutex>::log`, `base_sink.h:44`). Single-threaded, the commits just before and after
the fix take 356 and 338 ns per call. Multi-threaded measurements on this machine varied by 30 %
between identical runs and are not reported.

The first message on each thread, through a formatting sink:

```
1253a57d 2024-03-29 Add mdc support for default format                   first call: alloc 10 -> 11
bfa795e5 2026-10-24 Avoid creating the mdc thread-local context when it is unused (#3693)   first call: alloc 11 -> 10
```

The attribution at `1253a57d` names `__cxa_thread_atexit_impl` called from `mdc::get_context()`
(`mdc.h:29`) in `full_formatter::format`: every thread that logs constructs a thread-local map and
registers its destructor, whether or not it uses MDC. That is what the upstream fix describes; the
replay found the commit that introduced it, 31 months earlier, and the releases that carry it (v1.14.0
to v1.17.0).

## Case 3: the same FIX encoder, 2.8x apart between compilers

hffix writes and reads FIX messages without allocating, and its counts never changed. Its latency
depends strongly on the compiler. Encoding one New Order Single message (`replay/bench/hffix.cpp`),
ns per call, median of three interleaved runs on one idle core:

| | GCC 13 | GCC 15 | clang 21 |
|---|---:|---:|---:|
| -O2 | 384 | 301 | 342 |
| -O3 | 178 | 137 | 344 |

The fastest and the slowest build differ by 2.8x. Over the six builds, reading the message back
(`read_all`) ranges from 315 to 444 ns, and the checksum from 30 to 111 ns.

What differs is inlining. Every field begins with its tag, a compile-time constant such as 54,
converted to ASCII by `details::itoa` (a division loop and a `std::reverse`). With GCC 13 at `-O2`,
the encoder (with its split-off cold part) is 366 instructions and calls `itoa<int>` out of line 14
times, so constant tags are converted at run time. At `-O3` it is 6,733 instructions with no call to
`itoa`: everything is inlined, the constant tags fold, and the division-by-10 sequences that remain are
for the run-time values. Clang 21 keeps `push_back_string`, `push_back_int` and the other writers out
of line at both levels, so the tag is not a constant where `itoa` runs.

Five inlining parameters differ between GCC 13's `-O2` and `-O3` (`cases/hffix/inline_params.sh`):

| GCC 13 build | ns | instructions | out-of-line `itoa` calls |
|---|---:|---:|---:|
| -O2 | 384 | 366 | 14 |
| -O2 `max-inline-insns-auto=30` (15 at -O2) | 321 | 546 | 0 |
| -O2 `max-inline-insns-single=200` (70) | 336 | 835 | 16 |
| -O2 `early-inlining-insns=14`, `inline-min-speedup=15` or `inline-heuristics-hint-percent=600`, each alone | 380 to 398 | 366 | 14 |
| -O2 with all five at their -O3 values | 261 | 1,000 | 0 |
| -O3 | 177 | 6,733 | 0 |

The inlining parameters alone recover 123 of the 207 ns between `-O2` and `-O3`; the rest comes from
what `-O3` does with the inlined code.

Two source changes, both checked to write the same bytes as the original for 200,000 random
messages, with all three compilers (`cases/hffix/same_bytes.cpp`):

- `inline`: `itoa` and `utoa` marked `always_inline`;
- `fast`: a run-time `itoa` that counts digits by comparison and writes two digits at a time from the
  end, with no reversal.

| build | original | `inline` | `fast` | both |
|---|---:|---:|---:|---:|
| GCC 13 -O2 | 384 | 335 | 344 | 332 |
| GCC 13 -O3 | 178 | 181 | 218 | 196 |
| GCC 15 -O2 | 301 | 232 | 262 | 229 |
| GCC 15 -O3 | 137 | 134 | 152 | 110 |
| clang 21 -O2 | 342 | 349 | 317 | 312 |
| clang 21 -O3 | 344 | 326 | 254 | 286 |

(`cases/hffix/run.sh`; full output with the reader and the checksum in `cases/hffix/RESULT.txt`.)

No single change is best for every compiler. `fast` helps clang and hurts GCC at `-O3`; probably its
digit-counting loop folds less well for a constant tag than the original loop does, but that was not
checked. `inline` helps GCC at `-O2` and is neutral or better
elsewhere. The checksum (`calculate_check_sum`, a `std::accumulate` over the message bytes) is
vectorised (256-bit registers) by GCC 15 and clang at `-O2` but not by GCC 13 at `-O2`, which makes
it about 3.5x slower there.

## Case 4: MPMCQueue, a 64-bit division that clang shortens and GCC repeats

In the matrix, rigtorp's MPMCQueue `try_push`+`try_pop` takes 52 ns with GCC and 30 ns with clang,
stable in every round. The queue maps a ticket to a slot and a turn with `i % capacity_` and
`i / capacity_`, a 64-bit division by a run-time value.

- **GCC divides twice per operation.** One `divq` sits before the `lock cmpxchg` and a second one
  after it, for `turn(head)` in the final store. The atomic operation is a compiler barrier, so
  `capacity_` is loaded again and the quotient is not reused.
- **Clang divides on a shorter path.** LLVM's x86 back end bypasses slow 64-bit division: it emits
  `or` and `shr $32` to test whether both operands fit in 32 bits and, if they do, uses a 32-bit
  `div`, which is much cheaper on this core. Turning that off (`-Xclang -target-feature -Xclang
  -idivq-to-divl`) makes clang's build take 47.7 ns, as slow as GCC's.

A source change computes the turn once, before the atomic operations, and reuses it after them
(`cases/mpmcqueue/patch.py`, four lines per operation). An open pull request upstream (#36, 2022)
proposes a power-of-two capacity with a bit mask instead; that removes the division but changes what
a capacity means, while this change keeps both. A 4-producer, 4-consumer stress test with
capacity 1000 delivers all 8 million values exactly once with both versions
(`cases/mpmcqueue/stress.cpp`). ns per call, median of three runs (`cases/mpmcqueue/RESULT.txt`):

| build | push+pop | try_push+try_pop | divisions in `try_push` |
|---|---:|---:|---:|
| GCC 13 original | 75.1 | 52.5 | 2 |
| GCC 13 patched | 57.5 | 29.6 | 1 |
| GCC 15 original | 73.8 | 52.9 | 2 |
| GCC 15 patched | 57.8 | 29.6 | 1 |
| clang 21 original | 58.3 | 30.3 | 6 (3 pairs of 64- and 32-bit) |
| clang 21 original, bypass off | 75.3 | 47.7 | 3 |
| clang 21 patched | 49.4 | 26.6 | 4 |

The gain is on the uncontended path. Under contention (the stress test, eight threads), cache-line
transfers dominate, and the wall time was the same within run-to-run variation (1.39 to 1.48 s, both
versions). In one of the two complete runs of the script, two of the clang rows (patched, and bypass
off) came out about twice as slow as in every other run of the same binaries. The table is from the
run that agrees with fifteen separate runs of each binary.

## Static and run-time, side by side

The counts say what a benchmark does; they cannot see a path it does not take. A static analysis sees
every path but can only say "may". `replay/combine.py` runs both on the same commit, with clang for
both: `fn-effects` (`static/`) on the driver, and the benchmark
with call-stack recording. It then matches the two lists of allocation and lock sites per entry
function, in three groups:

- **exercised**: both agree, with the run-time count per call;
- **not exercised**: reachable according to the static analysis, never reached by the benchmark, with
  the analysis' hot/cold tag;
- **static miss**: reached at run time, absent from the static result.

Liquibook, adding an order (`combine.py liquibook 25b0c5ec` and `dd5ada83`):

```
before the fix (25b0c5e)
   exercised (5):
      alloc src/book/depth.h:418        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/depth.h:433        Depth<5>::find_level      per call 0.50   (library)
      alloc src/book/order_book.h:654   OrderBook<..>::add_order  per call 0.50   (line)
      alloc src/book/order_book.h:665   OrderBook<..>::add_order  per call 0.50   (line)
      alloc src/book/order_book.h:1049  OrderBook<..>::callback_now  per call 1.00   (line)
   not exercised: 22 not marked cold, 19 cold
   static miss (0)
after the fix (dd5ada8)
   exercised (4): the same, without callback_now
   not exercised: 23 not marked cold (callback_now's reserve among them), 20 cold
```

The static list is the same before and after; the fix moved one site from "exercised" to "not
exercised". The 22 hot sites the benchmark never reaches are on paths a benchmark of resting orders
does not take: stop orders (`order_book.h:557-580`), matching of all-or-none orders (`769-919`),
creating a trade (`1008`), and growth of the callback vector beyond its reserved 16 entries
(`397-427`). Each is a place where an order allocates in production that this benchmark would not
show.

concurrentqueue, `try_enqueue` without a token (latest commit): all four run-time events of the first
call match a static site, and one hot site is never reached:

```
   exercised (4):
      alloc concurrentqueue.h:2457  ImplicitProducer::ImplicitProducer   first call 1   (tail)
      alloc concurrentqueue.h:3524  get_or_add_implicit_producer          first call 1   (line)
      alloc concurrentqueue.h:642   ThreadExitNotifier::subscribe         first call 1   (caller)
      lock  concurrentqueue.h:3533  get_or_add_implicit_producer          first call 1   (line)
   not exercised: 1 not marked cold
      alloc concurrentqueue.h:3495  get_or_add_implicit_producer  [hot]   (growing the producer hash: many threads)
   static miss (0)
```

Matching the two views is the hard part, and the rule used is printed for each site:

- **line**: the innermost library frame of the run-time stack is the static site's line.
- **caller**: an outer frame is. The static analysis records a site in an inlined callee at its
  caller's line, here the TLS destructor registration of `subscribe` at line 3533.
- **tail**: the innermost frame's source line calls the function that holds the static site. The
  callee's own frame is missing from the run-time stack: `new_block_index()` at line 2457 holds the
  `malloc` that the static analysis reports at line 3000.
- **library**: the site is inside a standard-library function that was not inlined, such as a
  `std::map` insertion, and is matched by the library function that calls it.

A first version matched only the innermost frame. It reported two "static misses" in concurrentqueue
that were not misses: they were the caller and tail-call cases above.

## The compiler matrix

The latest library commit of every library, three compilers, two levels, ns per call (median of three
interleaved rounds on one idle core; quill's backend thread on a second core). Counts were identical
between all builds.

| library | operation | GCC 13 -O2 | GCC 13 -O3 | GCC 15 -O2 | GCC 15 -O3 | clang 21 -O2 | clang 21 -O3 | slowest / fastest |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| liquibook | add+cancel | 455 | 349 | 435 | 437 | 420 | 454 | 1.30 |
| concurrentqueue | try_enqueue+try_dequeue | 66 | 58 | 60 | 59 | 64 | 63 | 1.13 |
| concurrentqueue | try_enqueue_tok+try_dequeue_tok | 33 | 33 | 31 | 31 | 30 | 30 | 1.11 |
| concurrentqueue | enqueue+try_dequeue | 51 | 50 | 50 | 48 | 55 | 55 | 1.13 |
| atomic_queue | AtomicQueue push+pop | 41 | 40 | 43 | 43 | 42 | 42 | 1.07 |
| atomic_queue | AtomicQueue2 try_push+try_pop | 37 | 37 | 37 | 37 | 38 | 37 | 1.03 |
| atomic_queue | AtomicQueueB2 try_push+try_pop | 43 | 42 | 41 | 41 | 36 | 36 | 1.21 |
| SPSCQueue | try_push+consume | 10 | 10 | 10 | 9 | 9 | 10 | 1.11 |
| SPSCQueue | try_emplace+front+pop | 10 | 11 | 10 | 10 | 9 | 9 | 1.23 |
| MPMCQueue | push+pop | 75 | 76 | 74 | 74 | 56 | 57 | 1.35 |
| MPMCQueue | try_push+try_pop | 52 | 52 | 53 | 53 | 30 | 28 | 1.90 |
| hffix | write_new_order | 384 | 178 | 300 | 138 | 342 | 346 | 2.78 |
| hffix | read_all | 375 | 315 | 444 | 361 | 412 | 399 | 1.41 |
| hffix | check | 111 | 30 | 32 | 31 | 33 | 33 | 3.74 |
| spdlog | null sink info(int, double) | 142 | 148 | 139 | 150 | 147 | 148 | 1.08 |
| spdlog | null sink info(string) | 76 | 81 | 68 | 73 | 64 | 64 | 1.27 |
| spdlog | null sink debug below level | 12 | 13 | 11 | 11 | 6 | 6 | 2.07 |
| spdlog | ostream sink info(int, double) | 307 | 293 | 278 | 291 | 288 | 290 | 1.10 |
| fmt | format_to(memory_buffer) | 88 | 90 | 85 | 88 | 89 | 87 | 1.06 |
| fmt | format int (short string) | 35 | 34 | 32 | 31 | 28 | 28 | 1.25 |
| fmt | format double {:.4f} | 151 | 151 | 138 | 144 | 155 | 157 | 1.14 |
| fmt | format long string | 153 | 159 | 148 | 150 | 150 | 152 | 1.08 |
| simdjson | dom parse, reused parser | 222 | 221 | 217 | 213 | 196 | 199 | 1.13 |
| quill | LOG_INFO int, double | 16 | 15 | 15 | 14 | 14 | 15 | 1.13 |
| quill | LOG_INFO std::string | 18 | 19 | 16 | 16 | 17 | 17 | 1.14 |
| quill | LOG_DEBUG below level | 3 | 3 | 3 | 3 | 7 | 7 | 2.77 |
| readerwriterqueue | try_enqueue+try_dequeue | 10 | 10 | 10 | 10 | 10 | 11 | 1.17 |
| readerwriterqueue | enqueue+try_dequeue | 20 | 12 | 10 | 11 | 12 | 14 | 1.99 |
| unordered_dense | find (hit) | 18 | 18 | 28 | 19 | 21 | 20 | 1.53 |
| unordered_dense | find (miss) | 16 | 16 | 27 | 16 | 16 | 16 | 1.71 |
| unordered_dense | insert+erase | 46 | 46 | 44 | 44 | 48 | 48 | 1.11 |
| unordered_dense | operator[] (hit) | 17 | 17 | 15 | 15 | 17 | 16 | 1.15 |
| robin-map | find (hit) | 18 | 18 | 18 | 18 | 18 | 19 | 1.07 |
| robin-map | find (miss) | 21 | 21 | 22 | 21 | 22 | 23 | 1.08 |
| robin-map | insert+erase | 39 | 40 | 41 | 40 | 46 | 45 | 1.18 |
| robin-map | operator[] (hit) | 18 | 20 | 21 | 19 | 21 | 21 | 1.17 |
| parallel-hashmap | find (hit) | 19 | 19 | 18 | 18 | 18 | 18 | 1.08 |
| parallel-hashmap | find (miss) | 17 | 17 | 16 | 16 | 16 | 16 | 1.04 |
| parallel-hashmap | insert+erase | 70 | 71 | 69 | 70 | 70 | 70 | 1.02 |
| parallel-hashmap | operator[] (hit) | 18 | 18 | 18 | 18 | 23 | 23 | 1.26 |
| robin-hood-hashing | find (hit) | 35 | 35 | 35 | 35 | 37 | 37 | 1.06 |
| robin-hood-hashing | find (miss) | 20 | 20 | 20 | 19 | 21 | 21 | 1.07 |
| robin-hood-hashing | insert+erase | 101 | 99 | 101 | 97 | 106 | 103 | 1.10 |
| robin-hood-hashing | operator[] (hit) | 40 | 38 | 40 | 37 | 43 | 43 | 1.16 |
| fast_float | price 187.25 | 25 | 24 | 21 | 19 | 17 | 22 | 1.49 |
| fast_float | integer 100 | 12 | 10 | 12 | 11 | 14 | 16 | 1.60 |
| fast_float | 17 digits, exponent | 27 | 27 | 31 | 30 | 27 | 27 | 1.18 |
| fast_float | 4567.123456789012 | 22 | 23 | 21 | 20 | 21 | 21 | 1.11 |
| xxHash | XXH3_64bits 16 B | 7 | 8 | 7 | 8 | 7 | 7 | 1.18 |
| xxHash | XXH3_64bits 64 B | 14 | 15 | 13 | 13 | 12 | 10 | 1.55 |
| xxHash | XXH3_64bits 1024 B | 48 | 48 | 50 | 46 | 50 | 50 | 1.09 |
| xxHash | XXH64 64 B | 19 | 14 | 18 | 14 | 16 | 16 | 1.39 |
| xxHash | XXH64 1024 B | 113 | 107 | 110 | 107 | 113 | 108 | 1.06 |
| eigen | Matrix4d * Matrix4d | 15 | 15 | 15 | 15 | 15 | 15 | 1.05 |
| eigen | 8x8 matrix * vector | 35 | 38 | 45 | 50 | 37 | 37 | 1.42 |
| eigen | dot of 256 doubles | 41 | 38 | 41 | 41 | 37 | 35 | 1.18 |
| eigen | 4x4 Cholesky solve | 196 | 186 | 191 | 183 | 232 | 211 | 1.27 |

A ratio near 1.1 is within what repeated rounds of the same build differ by for the allocating
operations (Liquibook's spread between rounds was up to 83 ns); the tight operations repeat to within
1 ns. The large, stable ratios are hffix (case 3), MPMCQueue (case 4), and the disabled-level paths
of the two loggers, where clang is 2x faster than GCC for spdlog and 2x slower for quill (3 against 7
ns). Among the numerical and hashing libraries the differences that repeat in every round are smaller
but real: Eigen's 8x8 matrix-vector product takes 45 to 50 ns with GCC 15 against 30 to 38 with GCC 13
and 37 with clang; Eigen's 4x4 Cholesky solve is about 20 % slower with clang; XXH64 over 64 bytes
takes 19 ns with GCC 13 at `-O2` and 14 at `-O3`; parallel-hashmap's `operator[]` takes 23 ns with
clang against 18 with GCC. None of these was investigated further. fast_float's differences did not
repeat between rounds and are noise.

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
  covers paths the benchmark never runs, such as an order that trades or a queue that is full. The two answer different questions.
- **Timing every commit.** In the per-commit data, fast_float's parse time halves at one merge and
  doubles again at the next (`e3af1066`, `a7fbcb0a`), and readerwriterqueue alternates between 8 and
  14 ns eleven times. The two fast_float builds have the same machine code instruction for
  instruction, and timed alternately on an idle core they take the same time (14.9 and 14.9 ns).
  The jumps came from the load of the parallel replay. Throughput benchmarks varied by more than
  20 % between identical runs.
  One build of hffix's checksum took 109 ns or 155 ns on the same idle core depending on how busy
  the other cores were; the encoder took up to 55 % longer on a core whose hyperthread sibling was busy. Timings in
  this repository are medians of interleaved rounds on one physical core of an otherwise idle
  machine, and are compared only within one table.

## How it works

- `static/` is the LLVM pass plugin and its driver; `static/README.md` describes its rules, evidence
  levels, assumptions and tests.
- `replay/counts.h` replaces `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`, `memalign`,
  `posix_memalign` and `pthread_mutex_lock` in the benchmark executable (libstdc++'s `operator new`
  and `std::mutex` go through them) and counts per thread. With `COUNTS_TRACE=1` it records the
  return addresses of each event.
- `replay/bench_common.h` runs each operation once (the first-call counts), warms it up, then counts
  and times it in batches of 100 calls.
- `replay/drivers/<lib>/main.cpp` defines the operations as out-of-line entry functions with opaque
  arguments; `replay/bench/<lib>.cpp` is the measurement for each library.
- `replay/replay.py` builds the benchmark against a commit (`git archive`, nothing is installed),
  runs it pinned to a core, and stores one JSON line per operation. `--stride` samples, `--refine`
  bisects. `replay/diff.py` prints the changes; `replay/attribute.py` the stacks;
  `replay/matrix.sh` and `replay/matrix_table.py` the compiler matrix.
- `replay/combine.py` runs `static/fn-effects` on a library's driver and `attribute.py` on its
  benchmark at the same commit, both with clang, and matches the sites as described above.

## Using it on another library

A library needs two files: `replay/drivers/<lib>/main.cpp` with its hot operations as `HP_ENTRY`
functions, and `replay/bench/<lib>.cpp`, which calls them through `run_ops` (or one self-contained
benchmark file, as for spdlog). Then add an entry to `P` in `replay/replay.py` that names the paths
whose commits matter and the include directories. For a change under review, measuring the merge
base and the head is enough: the counts either differ or they do not, and `attribute.py` on the
head names the new line.

## What is not claimed

- **Counts are for the operations in the drivers, on the paths they take.** An allocation on a path
  the benchmark does not exercise (an error path, a full queue, a crossing order) is not counted.
- **Allocation and lock primitives outside the list above are not counted**: `mmap`, `futex` used
  directly, spin locks, `new` replaced by the library's own allocator. The first-call counts include
  one-time initialisation of whatever the process does first, so they depend on the order of the
  operations in the benchmark.
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
./fetch.sh                                   # clone the eighteen libraries into src/ (full history)
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

## License

Apache-2.0. The libraries are fetched from their own repositories and are not part of this one.
