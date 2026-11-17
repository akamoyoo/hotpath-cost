// Shared measurement loop: each operation is run WARM times, then MEAS times in batches; per operation it
// reports heap allocations, frees and mutex locks per call (exact counts on this thread) and median ns per
// call over batches of 100 calls.
#pragma once
#include "counts.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct Op { std::string name; std::function<void()> f; };

static void run_ops(const std::vector<Op> &ops, long warm = 10000, long meas = 200000) {
  for (auto &op : ops) {
    const bool trace = getenv("COUNTS_TRACE") != nullptr;
    if (trace) tl_trace = true;
    Counts f0 = counts_now(); op.f(); Counts f1 = counts_now();   // the first call, before any warm-up
    if (trace) { tl_trace = false; std::fprintf(stderr, "== %s, first call\n", op.name.c_str()); trace_dump(stderr, 1); }
    for (long i = 1; i < warm; i++) op.f();
    std::vector<double> t; t.reserve(meas / 100 + 1);
    Counts c0 = counts_now();
    for (long i = 0; i < meas; i += 100) {
      auto a = std::chrono::steady_clock::now();
      for (int k = 0; k < 100; k++) op.f();
      t.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count() / 100);
    }
    Counts c1 = counts_now();
    if (getenv("COUNTS_TRACE")) {   // one more pass with attribution, outside the timed loop
      const long tn = 1000;
      tl_trace = true; for (long i = 0; i < tn; i++) op.f(); tl_trace = false;
      std::fprintf(stderr, "== %s, steady state\n", op.name.c_str()); trace_dump(stderr, tn);
    }
    std::sort(t.begin(), t.end());
    std::printf("{\"op\":\"%s\",\"alloc\":%.4f,\"free\":%.4f,\"lock\":%.4f,\"ns\":%.1f,\"first_alloc\":%lu,\"first_lock\":%lu}\n", op.name.c_str(),
                double(c1.alloc - c0.alloc) / meas, double(c1.free - c0.free) / meas,
                double(c1.lock - c0.lock) / meas, t[t.size() / 2], (unsigned long)(f1.alloc - f0.alloc), (unsigned long)(f1.lock - f0.lock));
    std::fflush(stdout);
  }
}
