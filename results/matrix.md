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
