// jamesdbrock/hffix v1.4.1: "does no memory allocation on the free store". The README's own
// writer and reader examples, cut to the encode/decode operations (no I/O, no std::map dictionary).
#include <hffix.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "../hp_entry.h"

using TP = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

namespace hp_entry {
// New Order - Single, as in test/src/writer01.cpp; returns bytes written.
HP_ENTRY size_t fix_write_new_order(char *b, char *e, int seq, TP t, long px, int qty) {
  hffix::message_writer w(b, e);
  w.push_back_header("FIX.4.2");
  w.push_back_string(hffix::tag::MsgType, "D");
  w.push_back_string(hffix::tag::SenderCompID, "AAAA");
  w.push_back_string(hffix::tag::TargetCompID, "BBBB");
  w.push_back_int(hffix::tag::MsgSeqNum, seq);
  w.push_back_timestamp(hffix::tag::SendingTime, t);
  w.push_back_string(hffix::tag::ClOrdID, "A1");
  w.push_back_char(hffix::tag::HandlInst, '1');
  w.push_back_string(hffix::tag::Symbol, "OIH");
  w.push_back_char(hffix::tag::Side, '1');
  w.push_back_timestamp(hffix::tag::TransactTime, t);
  w.push_back_int(hffix::tag::OrderQty, qty);
  w.push_back_char(hffix::tag::OrdType, '2');
  w.push_back_decimal(hffix::tag::Price, px, -2L);
  w.push_back_char(hffix::tag::TimeInForce, '1');
  w.push_back_trailer();
  return w.message_end() - b;
}

// The reader loop of test/src/reader01.cpp: for every complete valid message, read fields.
HP_ENTRY long fix_read_all(const char *b, const char *e) {
  long acc = 0;
  hffix::message_reader r(b, e);
  for (; r.is_complete(); r = r.next_message_reader()) {
    if (!r.is_valid()) continue;
    hffix::message_reader::const_iterator i = r.begin();
    if (r.find_with_hint(hffix::tag::MsgSeqNum, i)) acc += i++->value().as_int<int>();
    if (r.find_with_hint(hffix::tag::Side, i)) acc += i++->value().as_char();
    if (r.find_with_hint(hffix::tag::OrderQty, i)) acc += i++->value().as_int<int>();
    int m = 0, x = 0;
    if (r.find_with_hint(hffix::tag::Price, i)) { i->value().as_decimal(m, x); acc += m + x; }
    TP t;
    if (r.find_with_hint(hffix::tag::TransactTime, i) && i->value().as_timestamp(t)) acc += t.time_since_epoch().count();
  }
  return acc;
}

// Checksum validation (the README: "does not do that calculation unless you explicitly ask for it").
HP_ENTRY bool fix_check(const char *b, const char *e) {
  hffix::message_reader r(b, e);
  return r.is_complete() && r.is_valid() && r.calculate_check_sum() == r.check_sum()->value().as_int<int>();
}
}

int main(int argc, char **) {
  static char buf[1 << 13];
  size_t n = hp_entry::fix_write_new_order(buf, buf + sizeof buf, argc, TP{}, 50001, 100);
  return (int)hp_entry::fix_read_all(buf, buf + n) + hp_entry::fix_check(buf, buf + n);
}
