// Writes 200000 messages with pseudo-random field values (ints of every sign and size, decimals,
// timestamps, chars, strings) and prints a hash of all bytes and the total length, so that two builds of
// hffix (original and patched) can be compared byte for byte. Also exercises the range-error path.
#include <hffix.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
using TP = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;
int main() {
  std::mt19937_64 rng(7);
  static char buf[1 << 12];
  uint64_t h = 1469598103934665603ull, total = 0; long errors = 0;
  auto pick = [&]() -> long {
    switch (rng() % 6) {
      case 0: return (long)(rng() % 10);
      case 1: return (long)(rng() % 100000) - 50000;
      case 2: return (long)rng();
      case 3: return std::numeric_limits<long>::max() - (long)(rng() % 3);
      case 4: return std::numeric_limits<long>::min() + 1 + (long)(rng() % 3);
      default: return -(long)(rng() % 1000);
    }
  };
  for (int m = 0; m < 200000; m++) {
    size_t cap = (m % 50 == 0) ? 40 + rng() % 60 : sizeof buf;   // some buffers too small: range errors
    try {
      hffix::message_writer w(buf, buf + cap);
      w.push_back_header("FIX.4.4");
      w.push_back_string(hffix::tag::MsgType, "D");
      // every random value is drawn in a statement of its own, so the sequence does not depend on the
      // compiler's order of evaluating function arguments
      int seq = (int)pick(); int tag = 10000 + (int)(rng() % 90000); long v = pick();
      unsigned qty = (unsigned)(rng() % 4000000000u); long mant = pick() / 7; long ex = -(long)(rng() % 9);
      char side = (char)('1' + rng() % 2); long long ms = (long long)(rng() % 4000000000000ull);
      w.push_back_int(hffix::tag::MsgSeqNum, seq);
      w.push_back_int(tag, v);
      w.push_back_int(hffix::tag::OrderQty, qty);
      w.push_back_decimal(hffix::tag::Price, mant, ex);
      w.push_back_char(hffix::tag::Side, side);
      w.push_back_timestamp(hffix::tag::TransactTime, TP{std::chrono::milliseconds(ms)});
      w.push_back_trailer();
      size_t n = w.message_end() - buf; total += n;
      for (size_t i = 0; i < n; i++) { h ^= (unsigned char)buf[i]; h *= 1099511628211ull; }
    } catch (std::exception &) { errors++; h ^= 0x9e3779b97f4a7c15ull * (uint64_t)m; }
  }
  std::printf("hash %016llx bytes %llu range_errors %ld\n", (unsigned long long)h, (unsigned long long)total, errors);
}
