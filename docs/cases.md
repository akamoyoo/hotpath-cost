# Cases

The six findings behind the summary in the [README](../README.md), with the commands that produce them, and the static and run-time views of the same commits. Paths are relative to the repository root.

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
depends strongly on the compiler. Encoding one New Order Single message (`replay/workloads/hffix.cpp`),
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

In the [compiler matrix](results.md#the-compiler-matrix), rigtorp's MPMCQueue `try_push`+`try_pop` takes 52 ns with GCC and 30 ns with clang,
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

## Case 5: Eigen 5's 4x4 Cholesky solve is 1.5x slower with GCC

Found by the instruction replay, not by timing releases. Eigen's history, sampled every 100th
commit and bisected on instructions per call:

```
28c3b26d 2025-04-12 masked load/store framework   (from cebe0911)
    4x4 Cholesky solve: instructions 548 -> 732 (+34%)
```

Timed alternately, nine rounds on one core, `x = A.llt().solve(b)` for a `Matrix4d` (`cases/eigen/run.sh`):

| | before (`cebe0911`) | after (`28c3b26d`) | ratio |
|---|---:|---:|---:|
| GCC 13 `-O2` | 206.0 ns | 307.4 ns | 1.49 |
| GCC 15 `-O2` | 159.9 ns | 224.0 ns | 1.40 |
| clang 21 `-O2` | 204.5 ns | 226.2 ns | 1.11 |

(The absolute times differ between runs because the machine was not idle; the two commits were always
timed in alternation, so the ratios are comparable.) Between the releases a user would upgrade across,
3.4.1 to 5.0.1, the same operation is 1.48x slower with GCC (149.9 against 222.1 ns) and 1.08x with clang,
while a 256-element dot product became 22 % faster. At the latest commit it is still 1.30x slower than
before the change with GCC.

`gap.py eigen eg_solve4 --commits cebe0911,28c3b26d` shows where: the solve's code grows from 361 to 540
instructions and 17 to 52 256-bit vector instructions, and a new out-of-line function appears,
`dense_assignment_loop_impl<...>` for the update `A.col(k).tail(rs) -= A.bottomLeftCorner(rs, k) *
A.row(k).head(k).transpose()` of the in-place factorisation. The blocks are of a fixed 4x4 matrix but
have run-time sizes of 1 to 3 elements. After the change, that update is a generic loop, not inlined,
built on the new masked-segment loads and stores (`Core/arch/AVX/PacketMath.h`); before it, the whole
solve was inlined into 361 instructions. Probably the masking costs more than it saves on vectors this
short; that was not measured separately. Not reported upstream yet (Eigen's tracker is on GitLab).

## Case 6: a random order flow reaches the paths, and finds an out-of-bounds write in Liquibook

Over Liquibook's five entry functions (add, add with conditions, immediate-or-cancel, cancel, replace),
the static analysis lists 85 allocation sites not marked cold; 19 of them are reachable from adding an
order with conditions. The fixed workload (resting orders, added and cancelled) reaches 5 of the 85.
An exploration workload (`replay/workloads/liquibook_explore.cpp`) replays a seeded random flow instead:
resting and crossing limit orders, market orders, stop orders, all-or-none, immediate-or-cancel and
fill-or-kill orders, cancels and replaces. `replay/combine.py liquibook-explore 2427613b32`:

```
== hp_entry::lb_add_cond(...)
   reached 10 of the 19 static sites not marked cold
   exercised (13): stop orders (order_book.h:591-629), all-or-none matching (829-979), trades,
                   depth levels, and one allocation by __cxa_allocate_exception (0.13 per call)
   not exercised: 9 not marked cold: growth of the callback vector (1109) and an error path
                  that the analysis did not mark cold (1249-1252)
   static miss (0)
...
all entries: reached 14 of 85 static sites not marked cold; 0 static misses
```

"exercised" lists run-time stacks, several of which can land on one static site or on a site marked
cold; "reached" counts static sites.

The allocation by `__cxa_allocate_exception` was an exception thrown inside the book, 0.13 times per
call over the traced calls: `std::runtime_error("Unexpected ignore_ask_fill_qty_")`, caught and printed by
`OrderBook::callback_now()`. Reducing the flow to the shortest prefix that still throws, by removing
actions while the exception remained, left ten orders and no cancel or replace. Tracing the depth book
through them showed `ignore_ask_fill_qty_` going from 0 to 2^64 - 1 with no call that sets it.

The cause is in `Depth<SIZE>::find_level(price, is_bid, false)`: when the price has no level and no
excess level, it returns its past-the-end pointer instead of `nullptr`. For bids that is the first ask
level; for asks it is one past `levels_`, where the members `last_change_`, `last_published_change_`,
`ignore_bid_fill_qty_` and `ignore_ask_fill_qty_` follow. Its two callers check for `nullptr` and then
modify the level. A triggered stop order is never added to the depth, so its fills arrive at a price
`find_level` does not find, and the write lands on those members. Setting `level = nullptr` before the
excess-level search fixes it; two new unit tests fail without the change (four failed checks) and pass
with it, the other 152 test cases give the same results before and after, and the 20,000-action flow
no longer throws. Reported and fixed upstream:
[liquibook#44](https://github.com/enewhuis/liquibook/issues/44),
[liquibook#45](https://github.com/enewhuis/liquibook/pull/45).

The tool was built to count allocations; this was a correctness bug, found because an exception is an
allocation and the exploration was looking for allocations the static analysis predicted.

## Case 7: concurrentqueue frees and reallocates its blocks under a varying load

concurrentqueue's fixed workload (one enqueue, one dequeue) never allocates after the first call: the
queue never holds more than its initial capacity. The static analysis lists six allocation sites
reachable from `enqueue`, among them `requisition_block`'s `create<Block>()` (`concurrentqueue.h:3138`).
An exploration workload (`replay/workloads/concurrentqueue_explore.cpp`) goes after them with a seeded
random sequence of bursts on one thread: enqueue and try_enqueue bursts of 1 to 3,000 elements, with and
without a producer token, and as many dequeue bursts, on a queue created with capacity 64. The queue's
length wanders up and down instead of growing.

`replay/combine.py concurrentqueue-explore 36c5755`: the block allocation is exercised, 1.05 times per
burst, and nothing is a static miss. Replayed at every library commit, the count changes once:

```
9c44f36d                       random bursts: alloc 0.13, free 0.00 per burst
a15a7236 2022-01-25            random bursts: alloc 1.05, free 1.05 per burst
         "Made implicit producers free dynamically allocated blocks all the way back to the heap by
          default (see issue #279)"
```

Before that commit, a block emptied by a dequeue went back to the queue's free list and the next
enqueue burst reused it; 0.13 allocations per burst are the queue growing to its largest length so
far. Since then, a block that was allocated beyond the initial capacity is freed as soon as it is
empty, and allocated again by the next burst that needs it: a `malloc` and a `free` per 32 elements,
in the steady state. This is deliberate and documented: the trait `RECYCLE_ALLOCATED_BLOCKS` (default
`false`) restores the old behaviour, and a queue sized for its peak never allocates. It is the kind of
cost the tool is for: invisible to a workload that stays within capacity, introduced by one commit,
and switched off by one line, once it is known.

The other five sites are the creation of an implicit producer for a thread (its block index, its entry
in the producer hash, the thread-exit callback). A single-threaded flow reaches them once, before
measurement; they are documented: `try_enqueue` "does not allocate memory (except for one-time implicit
producer)".

Two things in the tool came from this case. The first version of the flow only grew the queue, and its
"steady-state" allocations were growth; exploration workloads now keep the structure in a steady
state. And a cached result of that first version was compared with the second, which showed a change
at a commit that only removed `static` from inline functions; cached results now carry a fingerprint
of the workload and are set aside when it changes.

## Case 8: msgpack-c allocates twice per message when unpacking into a reused zone

The documented way to keep msgpack-c's unpacking off the heap is to reuse a `msgpack::zone`:
`z.clear(); msgpack::unpack(z, data, size)`. The workload (`replay/workloads/msgpack-c.cpp`) does that for a
four-field struct. The count at the latest commit is 2 allocations and 2 frees per message, and
`attribute.py msgpack-c bcba2717` names them:

```
  allocation 1.000 per call
      create_object_visitor::create_object_visitor(...)   include/msgpack/v2/create_object_visitor.hpp:31
      v2::detail::unpack_imp(...)                         include/msgpack/v2/unpack.hpp:348
  allocation 1.000 per call
      context<parse_helper<create_object_visitor>>::unpack_stack   include/msgpack/v2/parse.hpp:161
      v2::detail::parse_imp(...)                                   include/msgpack/v2/parse.hpp:1060
```

Both are the `reserve(MSGPACK_EMBED_STACK_SIZE)` of a `std::vector` parse stack that lives for one call. The
history replay of all 290 library commits pins the second one to the visitor API (`d15d9220`, 2016), which
split the parser's stack in two; before it, the count was 1.

Keeping the first 32 entries of each stack inline, and deeper levels in a vector, brings the reused-zone case
to 0 allocations and the `object_handle` case from 4 to 2. Timed alternately on one core (median of 9 rounds,
ns per call):

| | GCC 13 before | after | clang 21 before | after |
|---|---:|---:|---:|---:|
| unpack into a reused zone | 118.6 | 83.7 | 119.1 | 98.3 |
| unpack to an `object_handle` | 187.1 | 173.9 | 204.1 | 209.1 |

The last cell is 2 % slower, unexplained. The library's 40 tests pass, also under AddressSanitizer and
UBSan, with a new test across the inline/vector boundary. Reported and proposed upstream:
[msgpack-c#1222](https://github.com/msgpack/msgpack-c/issues/1222),
[msgpack-c#1223](https://github.com/msgpack/msgpack-c/pull/1223).

## Static and run-time, side by side

The counts say what a workload does; they cannot see a path it does not take. A static analysis sees
every path but can only say "may". `replay/combine.py` runs both on the same commit, with clang for
both: `fn-effects` (`static/`) on the driver, and the workload
with call-stack recording. It then matches the two lists of allocation and lock sites per entry
function, in three groups:

- **exercised**: both agree, with the run-time count per call;
- **not exercised**: reachable according to the static analysis, never reached by the workload, with
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
exercised". The 22 hot sites the workload never reaches are on paths a workload of resting orders
does not take: stop orders (`order_book.h:557-580`), matching of all-or-none orders (`769-919`),
creating a trade (`1008`), and growth of the callback vector beyond its reserved 16 entries
(`397-427`). Each is a place where an order allocates in production that this workload would not
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
