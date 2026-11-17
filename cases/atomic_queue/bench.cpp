// Benchmarks of the real atomic_queue code at one commit (the include path selects the commit).
//   single : one thread, push then pop, uncontended (the fast path)
//   pingpong: two threads, round trip through two queues (latency, cache line moves every time)
//   stream  : one producer, one consumer, 10^7 elements (throughput)
// Usage: bench MODE CPU_A CPU_B REPS ; prints the median of REPS runs in ns per operation.
#include <atomic_queue/atomic_queue.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <vector>

using Q = atomic_queue::AtomicQueue<uint32_t, 1024>;
using Q2 = atomic_queue::AtomicQueue2<uint64_t, 1024>;

static void pin(int cpu) {
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
  pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}
static double now() { return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

template <class QQ, class T> double single(int a) {
  pin(a); static QQ q; const long N = 20000000; T v{};
  double t0 = now();
  for (long i = 0; i < N; i++) { q.push(T(i + 1)); v += q.pop(); }
  double t = now() - t0;
  if (v == 0) std::puts("");
  return t / N;
}

template <class QQ, class T> double pingpong(int a, int b) {
  static QQ q1, q2; const long N = 2000000;
  std::thread th([&] { pin(b); for (long i = 0; i < N; i++) q2.push(q1.pop()); });
  pin(a); double t0 = now();
  for (long i = 0; i < N; i++) { q1.push(T(i + 1)); (void)q2.pop(); }
  double t = now() - t0; th.join();
  return t / N;  // one round trip = 2 push + 2 pop
}

template <class QQ, class T> double stream(int a, int b) {
  static QQ q; const long N = 10000000; T sum{};
  std::thread th([&] { pin(b); for (long i = 0; i < N; i++) sum += q.pop(); });
  pin(a); double t0 = now();
  for (long i = 0; i < N; i++) q.push(T(i + 1));
  th.join(); double t = now() - t0;
  if (sum == 0) std::puts("");
  return t / N;
}

int main(int argc, char **argv) {
  const char *m = argv[1]; int a = atoi(argv[2]), b = atoi(argv[3]), reps = atoi(argv[4]);
  bool two = argc > 5 && !strcmp(argv[5], "q2");
  std::vector<double> r;
  for (int i = 0; i < reps; i++) {
    double x = !strcmp(m, "single") ? (two ? single<Q2, uint64_t>(a) : single<Q, uint32_t>(a))
             : !strcmp(m, "pingpong") ? (two ? pingpong<Q2, uint64_t>(a, b) : pingpong<Q, uint32_t>(a, b))
             : (two ? stream<Q2, uint64_t>(a, b) : stream<Q, uint32_t>(a, b));
    r.push_back(x);
  }
  std::sort(r.begin(), r.end());
  std::printf("%.2f %.2f %.2f\n", r[r.size() / 2], r.front(), r.back());  // median min max
}
