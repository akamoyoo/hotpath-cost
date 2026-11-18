# The libraries

What the tool reported for every library it was applied to, then the compiler matrix. Paths are relative to the repository root. Per-commit data: `results/<build>/<library>.csv`; the matrix as Markdown: `results/matrix/matrix.md`.

## What was found

Forty-two libraries, 27,997 library commits in their first-parent histories, 6,397 of them measured.
Twenty-four of them were measured at every library commit where the driver compiles. quill was sampled
every fifth commit, spdlog every 30th, fmt every 50th, simdjson, rapidjson and flatbuffers every 4th, Eigen
every 100th, json and cereal every 10th, Taskflow and abseil every 15th, xsimd every 30th, glaze and Highway
every 40th, and every interval whose counts differed was bisected. Built with GCC 13.3, `-O2 -march=x86-64-v3`. Per-commit data:
`results/gcc/<library>.csv`.

| library | measured | count changes | what changed |
|---|---|---|---|
| [Liquibook](https://github.com/enewhuis/liquibook) (order book) | 13 of 60 | 2 | +2 allocations per add+cancel in release 2.0.0; fixed after it, never released (case 1) |
| [spdlog](https://github.com/gabime/spdlog) (logger) | 80 of 1,856, bisected | 6 | a mutex lock per message on `null_sink_mt` in v1.0.0 to v1.3.1; an allocation and a TLS destructor on each thread's first message in v1.14.0 to v1.17.0, fixed upstream in October 2026 (case 2) |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue) | 118 of 136 | 1 | the first `try_enqueue` of each thread takes a mutex since a 2022 use-after-free fix (`3e72a15`); it also allocates 3 times, before and after |
| [simdjson](https://github.com/simdjson/simdjson) | 53 of 272, bisected | 3 | first parse with a new parser: 6, then 10, then 11, then 6 allocations again (buffers); 0 per parse after it throughout |
| [quill](https://github.com/odygrd/quill) (logger) | 125 of 333 | 0 | `LOG_INFO`: 0 allocations, 0 locks, first call included |
| [fmt](https://github.com/fmtlib/fmt) | 61 of 3,407 | 0 | `format_to(memory_buffer)`: 0; `fmt::format` of a 41-character string: 1 (the `std::string` itself) |
| [atomic_queue](https://github.com/max0x7ba/atomic_queue) | 116 of 119 | 0 | 0 and 0 (`3ae8dff` crashes the workload) |
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
| [nlohmann/json](https://github.com/nlohmann/json) (JSON) | 115 of 822, bisected | 11 | allocations to parse one 90-byte message: 22 (2018), up to 26, down to 14 in 2026; one step back within that work (`fa37b952`, 17 -> 18); `dump()`: 5 -> 1 |
| [Taskflow](https://github.com/taskflow/taskflow) (task scheduling) | 428 of 1,069, bisected | 20 | submit and wait: 3 to 5 allocations and 2 to 4 locks per call over the history; now 3 and 2 (`run`), 4 and 1 (`async`) |
| [pdqsort](https://github.com/orlp/pdqsort) (sorting) | 25 of 37 | 0 | 0 and 0 |
| [lz4](https://github.com/lz4/lz4) (compression) | 293 of 298 | 0 | 0 and 0 |
| [dragonbox](https://github.com/jk-jeon/dragonbox) (double to decimal) | 196 of 210 | 0 | 0 and 0 |
| [libdivide](https://github.com/ridiculousfish/libdivide) (division by a run-time constant) | 163 of 163 | 0 | 0 and 0; 64 divisions in 115 ns against 670 with the hardware divider |
| [date](https://github.com/HowardHinnant/date) (time stamps) | 139 of 141 | 0 | parsing through `istringstream` and formatting: 2 allocations each, throughout |
| [fmtlog](https://github.com/MengRao/fmtlog) (low-latency logger) | 48 of 51 | 1 | `logi`: 0 and 0 per call; the first call on a thread: 3 -> 2 allocations and locks in 2021 (`692db5eb`, "optimize threadbuffer mutex holding time") |
| [bitsery](https://github.com/fraillt/bitsery) (binary serialization) | 11 of 94 | 0 | 0 and 0 (older versions do not build with the driver) |
| [cereal](https://github.com/USCiLab/cereal) (serialization) | 17 of 143, sampled | 1 | through `std::stringstream`: 2 allocations to serialize, 1 to deserialize; 4 -> 2 for serializing in 2013 |
| [xsimd](https://github.com/xtensor-stack/xsimd) (SIMD) | 15 of 859, sampled | 0 | 0 and 0 |
| [msgpack-c](https://github.com/msgpack/msgpack-c) (MessagePack, C++) | 193 of 290 | 3 | unpacking into a reused zone: 2 allocations per message outside the zone, the parse stacks; 1 before the visitor API (2016, `d15d9220`). Fix proposed upstream (case 8) |
| [flatbuffers](https://github.com/google/flatbuffers) (serialization) | 134 of 799, bisected | 1 | building with a reused builder: 0; a new builder per message: 3 -> 1 allocations in 2017 (`79b80f84`, "Reduced FlatBufferBuilder from 3 buffers to 1") |
| [rapidjson](https://github.com/Tencent/rapidjson) (JSON) | 89 of 443, bisected | 2 | DOM parse of a 90-byte message: 5 -> 7 allocations (2015), 7 -> 8 in 2021 (`cd5ee4df`, #1858: the pool allocator allocates a shared control block in its constructor), 587 -> 616 ns; on a fixed buffer: 0 |
| [yyjson](https://github.com/ibireme/yyjson) (JSON) | 239 of 280 | 0 | parse: 2 allocations with the default allocator, 0 with a pool, throughout |
| [glaze](https://github.com/stephenberry/glaze) (JSON) | 45 of 2,689, sampled | 1 | reading into a reused struct and writing to a reused string: 0; first read 4 -> 2 allocations in 2024 (`f8cb7923`, statically sized decode buffer) |
| [abseil](https://github.com/abseil/abseil-cpp) (`flat_hash_map`) | 69 of 1,197, sampled | 0 | 0 and 0 (the 2019 versions that sample every table by default are not built: the workload does not link the sampler). Instructions, 179 commits: 24 changes of 5 % or more since 2025, mostly refactorings followed by a fix within weeks: `f2f4fa2c` +50 % per `operator[]` (+37 % time), `179d2e39` -32 % ten days later; `3b23f75e` +40 %, `762b5238` -25 %. Net since October 2018 (`c16d5557`): insert+erase 193 -> 278 instructions, 61 -> 69 ns with GCC 13 and 61 -> 70 with clang 21; find of a missing key +4 % and +12 %; find of a present key no slower (100,000 `uint64_t` keys) |
| [Aeron](https://github.com/real-logic/aeron) (messaging; the C client's ring buffers and broadcast buffer) | 237 of 393 | 0 | 0 and 0 at every measured commit since December 2020: the zero-allocation, lock-free design holds. Instructions: the broadcast receive +8 % (`2dfe1446`, 2024, an expandable scratch buffer), the one-to-one ring buffer -14 % (`b322e972`, 2026). Commits before December 2020 lack `try_claim` or the broadcast headers the workload uses |
| [oneTBB](https://github.com/uxlfoundation/oneTBB) (concurrent containers, tasks) | 345 of 346 | 5 | steady state unchanged since 2020: `concurrent_queue` mallocs and frees a page per 32 elements with at most one element queued (pages are not cached; with tbbmalloc, the usual allocator in deployment, this is cheaper), `concurrent_hash_map` allocates a node per insert, `task_group` run+wait allocates nothing after its first call. The 5 changes are all in the first call's set-up (40 -> 55 allocations over six years). Instructions not compared: a worker thread |
| [Boost.JSON](https://github.com/boostorg/json) | 420 of 836 | 3 | parsing into a `monotonic_resource` on a fixed buffer, and with a reused parser: 0 allocations at every commit since September 2020 (before the first release); the default parse made 9 allocations instead of 8 from "Refactor object" (`844851c6`, October 2020) to "do not reserve preemptively in object" (`c4329519`, June 2023), releases 1.75 to 1.82. Instructions: 3 changes, the largest -12 % for serializing doubles with Boost.Charconv (2024) |
| [Boost.Unordered](https://github.com/boostorg/unordered) `unordered_flat_map` | 210 of 855 (the container exists since October 2022) | 0 | 0 and 0. Instructions: 5 changes, four of them before its first release (hash mixing, strong exception guarantee, and inlining fixes); the concurrent version's merge (`d65d9a65`, 2023) left the sequential `operator[]` 9 % more instructions and 5 % slower with GCC |
| [Boost.Unordered](https://github.com/boostorg/unordered) `unordered_map` | 692 of 855, since 2008 | 0 | 1 allocation per insert+erase, throughout. Instructions: 8 changes; the bucket policy for integer keys used two 64-bit divisions per lookup from 2014 (`99fdce0b`, a typedef fix that applied the intended policy) to 2022 (`d6576ed2`, after #50): 13 to 36 % slower in this workload, with 32 % fewer instructions (docs/insights.md, lesson 2) |
| [gtl](https://github.com/greg7mdp/gtl) (hash map) | 179 of 179 | 0 | 0 and 0; instructions per call unchanged at every other commit |
| [emhash](https://github.com/ktprime/emhash) (hash map) | 219 of 228 | 0 | 0 and 0. Instructions per `find`: +6 to +7 % in June 2026 at `ba124419` "fix:overflow", which also turned on a prefetch, and back a month later at `280842c5` "disable prefetch by default ... (no performance gain)" |
| [Highway](https://github.com/google/highway) (SIMD) | 36 of 2,329, sampled | 0 | 0 and 0; instructions per 1,024-float sum and dot unchanged |

"Measured" counts the commits at which the fixed workload compiles and runs; older versions
with a different API are not measured. The quiet rows are results too: these libraries keep their
hot path at zero allocations and locks, and over every measured commit nothing slipped in. For the
numerical and hashing libraries the counts are zero by design; what differs there is time, which the
compiler matrix below compares.

## Six JSON libraries, one message

The JSON workloads parse the same 90-byte trade message
(`{"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]}`) and read one
field. Latest library commit of each, GCC 13 `-O2 -march=x86-64-v3`; allocations and instructions per
call are exact; times are medians of five rounds run alternately on one idle core.

| library | how | allocations | instructions | ns |
|---|---|---:|---:|---:|
| yyjson | default allocator | 2 | 1,589 | 227 |
| yyjson | pool allocator | 0 | 1,390 | 196 |
| simdjson | DOM, reused parser | 0 | 1,857 | 229 |
| glaze | read into a reused struct (no DOM) | 0 | 1,573 | 193 |
| rapidjson | default allocator | 8 | 4,848 | 612 |
| rapidjson | memory pool on a fixed buffer | 0 | 3,189 | 394 |
| Boost.JSON | default resource | 8 | 6,351 | 869 |
| Boost.JSON | `monotonic_resource` on a fixed buffer | 0 | 5,009 | 632 |
| Boost.JSON | reused parser, fixed buffer | 0 | 4,737 | 587 |
| nlohmann/json | default | 14 | 8,010 | 1,187 |

Every library has a way to parse this message without the heap, and in every one of them that way has
held at every measured commit since it existed. What remains is the work per byte, which differs by a
factor of about four between the DOM parsers. In Boost.JSON's case no single place dominates: under
callgrind the largest function is 7 % of the instructions; the rest is the parser, the object's hash
index, string copies and the memory resource.

## Open pull requests

`replay/prs.py LIB` (`check.py LIB BASE HEAD` on every open pull request (November 17, 2026, up to 30 per library)
that touches a library's measured paths, BASE being its merge base): 148 pull requests of 14 libraries
(spdlog, fmt, simdjson, nlohmann/json, glaze, quill, msgpack-c, yyjson, parallel-hashmap, fast_float,
Taskflow, xxHash, rapidjson, flatbuffers; concurrentqueue's 8 checked first). 122 change no count and
no instruction count by 3 % or more; 2 do not build with the workload. The others, priced with
`price.py` where it matters (median of 9 alternating rounds on one core, GCC 13 `-O2`):

| pull request | counts | instructions | time |
|---|---|---|---|
| parallel-hashmap #167 "AVX2 optimization of group lookup" | same | find +6 %, `operator[]` +6 % | with AVX2 (`-march=x86-64-v3`), slower in every case of a standalone benchmark: find +3 to +9 %, find of a missing key in a 10-million-entry table +42 % (15.7 -> 22.3 ns), insert+erase +11 to +17 %, with GCC 13 and clang 21; unchanged without AVX2 |
| fast_float #307 "Improving speed and reduce code size ..." | same | `integer 100` +11 %, 17 digits with exponent -5 % | mixed in a standalone benchmark: with GCC 13, `100` and `65535` 4 to 7 % slower, other inputs up to 12 % faster; with clang 21, mostly faster (`100` 10 to 17 %) |
| fast_float #416 "Speed up parsing: branch-free exponent ..." | same | -3 to -4 % | `price 187.25` -9 %, others within 3 % |
| rapidjson #2402, #2403 (allocation failures) | same | +5 %, +8 % on a fixed-buffer pool | +5 %, +6 % |
| spdlog #3641 "Reuse backtrace ring-buffer slots ..." | same | +27 per message with backtrace off (+5 % for `info(string)`) | +4 % for `info(string)`, within noise otherwise |
| fmt #4997 "Add `'` format option for digit grouping" | same | `{:.4f}` of a double +4 % | +1.7 % |
| spdlog #3404 (compile-time patterns) | same | -3 % | not timed |
| msgpack-c #1103 | first unpack into a zone: 2 -> 3 allocations | | |
| msgpack-c #1223 (from this work) | unpack into a reused zone: 2 -> 0 allocations | | case 8 |

Checked later the same way (November 18): Boost.JSON's 17 open pull requests that touch its headers,
Aeron's 3 that touch the measured C sources, and 9 of oneTBB's (the run stopped at its time limit: each
oneTBB build compiles the whole library, and its instructions are measured three times because of the
worker thread). None changes a count or the instructions per call by 3 % or more; Boost.Unordered's one
open pull request does not touch its headers.

Taskflow's pull requests first showed instruction changes for most of them, including one that only
touches MSVC module builds: with a worker thread, one build's count varies by up to 8 % between runs.
`check.py` now measures such workloads three times and compares ranges, and none of them is left.
The first prices of the two rows above came from `price.py`, with the replay's workload: insert+erase
11 % faster for #167, `integer 100` 19 % slower for #307. A standalone benchmark (one `noinline` function,
21 batches of a million calls, alternating builds) gave insert+erase 11 to 17 % slower and `100` 7 %
slower. Both harnesses are exact about counts; their times differ with code layout and the loop around
the call. A price meant for someone else is now confirmed with a second, independent harness. The
standalone numbers for #167 were posted to the pull request
([comment](https://github.com/greg7mdp/parallel-hashmap/pull/167#issuecomment-6961404564)), which had
asked whether the change helps; nothing else was posted.

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
