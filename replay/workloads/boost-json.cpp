// boostorg/json: DOM parse of a small market-data message (the one the rapidjson and yyjson workloads
// use), with the default memory resource, into a monotonic_resource on a fixed buffer (the documented
// way to parse without the heap), with a parser reused across messages on such a resource, and
// serialization to a string. Header-only build (boost/json/src.hpp); the other Boost modules come from
// an installed release with its boost/json* hidden (sysinc in replay.py).
#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include "../drivers/hp_entry.h"
#include "../workload.h"
static const char MSG[] = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double bj_parse(boost::json::string_view s) {
  boost::json::value v = boost::json::parse(s); return v.as_object()["px"].as_double();
}
HP_ENTRY double bj_parse_fixed(boost::json::string_view s, unsigned char *buf, std::size_t n) {
  boost::json::monotonic_resource mr(buf, n);
  boost::json::value v = boost::json::parse(s, &mr); return v.as_object()["px"].as_double();
}
HP_ENTRY double bj_parser_reused(boost::json::parser &p, boost::json::string_view s, unsigned char *buf, std::size_t n) {
  boost::json::monotonic_resource mr(buf, n);
  p.reset(&mr); p.write(s.data(), s.size()); boost::json::value v = p.release();
  return v.as_object()["px"].as_double();
}
HP_ENTRY std::size_t bj_serialize(const boost::json::value &v) { return boost::json::serialize(v).size(); }
}
int main() {
  static unsigned char buf[8192];
  boost::json::string_view s(MSG, sizeof MSG - 1);
  boost::json::parser p; p.reset(); p.write(s.data(), s.size()); boost::json::value v = p.release();
  double acc = 0;
  run_ops({{"parse + read px", [&] { acc += hp_entry::bj_parse(s); }},
           {"parse into a fixed buffer", [&] { acc += hp_entry::bj_parse_fixed(s, buf, sizeof buf); }},
           {"reused parser, fixed buffer", [&] { acc += hp_entry::bj_parser_reused(p, s, buf, sizeof buf); }},
           {"serialize to std::string", [&] { acc += hp_entry::bj_serialize(v); }}});
  return acc == 1;
}
