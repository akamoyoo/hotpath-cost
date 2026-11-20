// quickfix/quickfix: the message layer of the FIX engine, without sessions or sockets: building a
// NewOrderSingle and serializing it, serializing a message that is reused, parsing a received order into
// a reused message without validation and with validation against the FIX 4.4 data dictionary
// (spec/FIX44.xml of the same commit), and reading two fields. The engine's message sources are
// compiled in (see P in replay.py); Allocator.h, which configure writes, is written empty (the default).
#include "fix44/NewOrderSingle.h"
#include "DataDictionary.h"
#include <string>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
HP_ENTRY std::size_t qf_build(int i) {
  FIX44::NewOrderSingle o(FIX::ClOrdID("ORD-1"), FIX::Side(FIX::Side_BUY), FIX::TransactTime(), FIX::OrdType(FIX::OrdType_LIMIT));
  o.set(FIX::Symbol("AAPL")); o.set(FIX::OrderQty(100)); o.set(FIX::Price(187.25 + (i & 7)));
  o.getHeader().setField(FIX::SenderCompID("CLIENT")); o.getHeader().setField(FIX::TargetCompID("EXCH"));
  o.getHeader().setField(FIX::MsgSeqNum(i + 1));
  return o.toString().size();
}
HP_ENTRY std::size_t qf_serialize(const FIX::Message &m, std::string &out) { m.toString(out); return out.size(); }
HP_ENTRY void qf_parse(FIX::Message &m, const std::string &s) { m.setString(s, false); }
HP_ENTRY void qf_parse_validated(FIX::Message &m, const std::string &s, const FIX::DataDictionary &dd) { m.setString(s, true, &dd); }
HP_ENTRY double qf_read(const FIX::Message &m) {
  FIX::Price p; FIX::OrderQty q; m.getField(p); m.getField(q); return p.getValue() + q.getValue();
}
}
int main() {
  FIX::DataDictionary dd(QF_SPEC);
  FIX44::NewOrderSingle o(FIX::ClOrdID("ORD-1"), FIX::Side(FIX::Side_BUY), FIX::TransactTime(), FIX::OrdType(FIX::OrdType_LIMIT));
  o.set(FIX::Symbol("AAPL")); o.set(FIX::OrderQty(100)); o.set(FIX::Price(187.25));
  o.getHeader().setField(FIX::SenderCompID("CLIENT")); o.getHeader().setField(FIX::TargetCompID("EXCH"));
  o.getHeader().setField(FIX::MsgSeqNum(1)); o.getHeader().setField(FIX::SendingTime());
  const std::string wire = o.toString();
  FIX::Message m(wire, dd, true), r; std::string out; double acc = 0; int i = 0;
  run_ops({{"build NewOrderSingle + toString", [&] { acc += hp_entry::qf_build(i++); }},
           {"toString of a reused message", [&] { acc += hp_entry::qf_serialize(m, out); }},
           {"parse into a reused message", [&] { hp_entry::qf_parse(r, wire); }},
           {"parse, validated (FIX44 dictionary)", [&] { hp_entry::qf_parse_validated(r, wire, dd); }},
           {"read Price and OrderQty", [&] { acc += hp_entry::qf_read(m); }}});
  return acc == 1;
}
