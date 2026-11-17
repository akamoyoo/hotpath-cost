# What we learned

Each point below was found by running the tool on real libraries, and each one changed how the tool
works. The README states them in one line each; this page has the evidence. Paths are relative to
the repository root.

## 1. Count, don't time

Allocations, frees, mutex locks and executed instructions per call are exact. For a given build
they are the same on every run and under any load, so a difference between two commits is a
difference in the code. Timing has no such property:

- Two adjacent fast_float commits (`e3af1066`, `a7fbcb0a`) are 2x apart in the per-commit replay data.
  Their machine code is the same instruction for instruction (3,730 instructions). Timed alternately
  on an idle core, both take 14.9 ns. The jump came from the load of the parallel replay.
- readerwriterqueue alternates between 8 and 14 ns eleven times over its history, with no relation
  to what the commits changed.
- One build of hffix's checksum took 109 ns or 155 ns on the same core, depending on how busy the
  other cores were. On a core whose hyperthread sibling was busy, the encoder took 55 % longer.
- Instruction counts of the same build repeat exactly: fast_float's four parses took 155, 97, 239 and
  198 instructions in each of three runs.

Exactness has one limit: it holds for work that the calling thread does itself. In a workload with
a worker thread (Taskflow's executor), frees counted on the calling thread vary between runs of the
same build, from 1.4 to 2.8 per call, because either thread may drop the last reference first. A first
replay of Taskflow's history reported 260 "changes", nearly all of them that. Allocations and locks
on the calling thread did repeat (with a rare 3.99 instead of 4.00 locks per call), and without frees
the history shows 20 changes, which match their commits ("removed the waiter vector in
AtomicNotifier": one lock fewer per call). The replay no longer compares frees when the workload has
another thread.

So the replay finds changes with counts and puts a price on them by timing only the two commits
concerned, alternately, on one idle physical core (`replay/price.py`).

## 2. Instruction counts find performance changes; they do not price them

Every change in instructions per call of 5 % or more that was timed afterwards moved time in the same
direction. The size of the effect on time varied by a factor of more than 20:

| change | instructions | time |
|---|---:|---:|
| fast_float PR #387, `integer 100` | -42 % | -39 % |
| fast_float PR #387, `price 187.25` | -28 % | -25 % |
| fast_float PR #387, 17 digits with exponent | -21 % | -18 % |
| unordered_dense `5d9416dd`, `operator[]` | -23 % | -14 % |
| unordered_dense `5d9416dd`, insert+erase | +17 % | +9 % |
| robin-map `68ff7325`, insert+erase | +26 % | +10 % |
| robin-hood-hashing `a8f297b4`, `operator[]` | +24 % | +8 % |
| xxHash `d9dfb820`, XXH64 over 64 bytes | -9 % | -6 % |
| xxHash `d9dfb820`, XXH64 over 1 KiB | -14 % | -1 % |
| Eigen, 4x4 Cholesky, GCC 13 `-O2` to `-O3` | -35 % | -5 % |
| Eigen, 8x8 matrix-vector, GCC 13 to GCC 15 at `-O2` | +13 % | +29 % |
| Eigen `28c3b26d`, 4x4 Cholesky solve, GCC 13 | +34 % | +49 % |

Code bound by a dependency chain (a multiply chain in XXH64, square roots and divisions in Cholesky)
hides most of an instruction change. Code bound by the number of instructions passes it on almost
one to one (fast_float). The Eigen cases go the other way: time grew more than instructions, up to twice
as much.
An instruction count is a detector, and the price has to be measured.

## 3. A machine-code difference is not a cost

Before any of the counting, the replay compared machine code across commits. In atomic_queue it found
`asm("" :: "r"(ptr))` hints that changed GCC's address arithmetic. They removed a three-component
`lea`, which is 3 cycles against 1 in a dependent chain on this core, and grew clang's code by 23 %.
The real queue, timed across the same commits, showed no difference outside the noise: the two `lock`
instructions of a push and a pop take about 28 ns and hide two cycles. A hand-written copy of the
sequence alone overstated the effect as 7 % (`cases/atomic_queue/`).

## 4. A compiler gap is a lead, and its cause is usually one of a few

When the same source is much faster with one compiler or level than with another, the fast build
proves that the speed is reachable. In the cases examined, the cause was always visible in the machine
code of the hot function and its callees (`replay/gap.py` lists these facts per build):

- **A helper left out of line.** hffix's encoder converts constant field tags with `details::itoa`.
  GCC at `-O2` calls it out of line 15 times per message and converts the constants at run time; at
  `-O3` it is inlined and the constants fold: 384 against 178 ns. GCC's `-O3` inlining parameters
  alone recover 123 of the 207 ns.
- **A repeated division.** MPMCQueue divides by its capacity twice per operation, because the atomic
  compare-and-swap between the two divisions forces the capacity to be reloaded. LLVM hides most of the
  cost by trying a 32-bit division first; GCC does not: 52.5 against 30.3 ns.
- **Vectorisation in one build only.** hffix's checksum (`std::accumulate` over bytes) is vectorised by
  GCC 15 and clang at `-O2` but not by GCC 13: 111 against 32 ns.

The useful fix is in the source, not in the compiler flags. It makes the code fast with every compiler:
computing MPMCQueue's turn once gave GCC 29.6 ns and clang 26.6 ns. But a source change tuned for one
compiler can cost another one: hffix's faster run-time `itoa` helped clang (-26 %) and slowed GCC
`-O3` (+22 %). Every candidate fix has to be timed with every compiler.

Not every gap was explained. GCC 15 executes about 20 % more instructions than GCC 13 inside Eigen's
matrix-vector kernel for an 8x8 product (247 against 206 per call), consistent with the slowdown. The
cause in the generated loop was not found.

## 5. The static and the run-time view each see what the other cannot

The static analysis sees every path but can only say "may". Liquibook's `callback_now` may allocate
both before and after the fix that removed an allocation from every order: the fix keeps a `reserve`
call and only makes it a no-op. The run-time count is the only thing that tells the versions apart
(4 against 2 allocations per add+cancel).

The run-time count sees only the paths the workload takes. For the same Liquibook operation, the
static analysis lists 22 sites, not marked cold, that the workload never reaches: stop orders,
matching of all-or-none orders, trade creation and growth of the callback vector. Each is a place
where an order allocates in production that the workload does not show.

Matching the two is where the work is (`replay/combine.py`). A first version matched only the
innermost frame of each run-time stack and reported two "static misses" in concurrentqueue that were
not misses: the static analysis records an inlined callee's site at its caller's line, and one
run-time stack had lost a frame to a tail call. With those rules, every allocation and lock seen at
run time in the measured Liquibook and concurrentqueue operations matches a static site.

## 6. The static view tells the workload where to go

The list of sites a workload never reaches is a list of targets. For Liquibook, a seeded random order
flow written to reach them (all order types, cancels and replaces, every action prepared before
measurement) reached 14 of the 85 sites not marked cold over the five entry functions, against 5 for
the fixed workload; for adding an order with conditions, 10 of 19, and the rest of those were the
growth of a vector beyond its reserved size and an error path. It also found a bug the tool was
not looking for: one of the exercised "allocations" was `__cxa_allocate_exception`, an exception thrown
inside the book during ordinary order flow. Reduced to ten orders and traced, it was an out-of-bounds
write in `Depth::find_level` (case 6 in `cases.md`). Counting allocations made the exception visible;
the static list made the exploration go where it was.

The same holds for concurrentqueue: a random sequence of enqueue and dequeue bursts reached the block
allocation the analysis listed for `enqueue`, and showed that since 2022 the queue frees and
reallocates a block per 32 elements whenever its length goes beyond its initial capacity and back
(case 7). A fixed enqueue-dequeue pair never leaves the initial capacity and never sees it.

A random flow is enough here because the operations of an order book are few and cheap to generate.
It does not need a fuzzing framework: a fixed seed keeps each run reproducible, and the existing call
stack recording and matching attribute everything it reaches.

## 7. Costs enter through refactorings, come back, and stay unreleased

- spdlog's null sink took a mutex until a 2016 commit removed it. A 2018 refactoring ("pattern per
  sink and pattern caching") put it back for six releases, and a one-line commit removed it again in
  2019.
- A 2024 feature in spdlog (MDC) made every thread's first message allocate and register a TLS
  destructor, whether or not it used the feature. It was fixed upstream in October 2026, eight
  releases later.
- Liquibook's extra allocation per order came in with a simplification and left the next day, but the
  release tag sits between the two commits, and there is no later release.
- concurrentqueue's first `try_enqueue` on each thread has taken a mutex since a 2022 fix for a
  use-after-free: a cost paid for correctness, visible only on the first call.

- concurrentqueue has, since a 2022 commit, freed each block allocated beyond its initial capacity as
  soon as it is empty, and allocated it again on the next burst: deliberate, documented, and switched
  off by a trait, but invisible to a workload that stays within capacity.

- Eigen's 4x4 Cholesky solve became 1.5x slower with GCC in April 2025, inside a 1,363-line commit
  that added a masked load/store framework. It shipped in 5.0.0 and 5.0.1 and is still there at the
  latest commit (1.30x).

None of these was a bug, and none was caught by tests. Each is a one-line difference in an exact count
at the commit that made it.
