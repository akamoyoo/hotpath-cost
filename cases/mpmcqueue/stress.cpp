// 4 producers and 4 consumers through one rigtorp::MPMCQueue of capacity 1000 (not a power of two),
// half of the operations blocking (push/pop) and half non-blocking (try_push/try_pop): every value
// must arrive exactly once. Prints the checksum of values and the count.
#include <rigtorp/MPMCQueue.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
int main() {
  const int P = 4, C = 4; const long N = 2000000;   // per producer
  rigtorp::MPMCQueue<long> q(1000);
  std::vector<std::atomic<unsigned char>> seen(P * N);
  std::atomic<long> got{0}, dup{0};
  std::vector<std::thread> t;
  for (int p = 0; p < P; p++) t.emplace_back([&, p] {
    for (long i = 0; i < N; i++) { long v = p * N + i; if (i & 1) q.push(v); else while (!q.try_push(v)) {} }
  });
  for (int c = 0; c < C; c++) t.emplace_back([&, c] {
    for (long k = 0; k < P * N / C; k++) {
      long v; if (k & 1) q.pop(v); else while (!q.try_pop(v)) {}
      if (seen[v].fetch_add(1)) dup++;
      got++;
    }
  });
  for (auto &x : t) x.join();
  long missing = 0; for (auto &s : seen) missing += s.load() == 0;
  std::printf("received %ld of %ld, duplicates %ld, missing %ld\n", got.load(), P * N, dup.load(), missing);
  return dup || missing;
}
