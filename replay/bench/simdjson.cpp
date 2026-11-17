// simdjson/simdjson (amalgamated singleheader): DOM parsing of a small market-data message with a reused
// parser, which the documentation says reuses its buffers.
#include "simdjson.h"
#include <cstring>
#include "../drivers/hp_entry.h"
static const char MSG[] = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double sj_dom(simdjson::dom::parser &p, const simdjson::padded_string &s) {
  simdjson::dom::element e;
  if (p.parse(s).get(e)) return -1;
  double px; if (e["px"].get(px)) return -2;
  return px;
}
}
#include "../bench_common.h"
int main() {
  simdjson::dom::parser p; simdjson::padded_string s(MSG, std::strlen(MSG)); double acc = 0;
  run_ops({{"dom parse, reused parser", [&] { acc += hp_entry::sj_dom(p, s); }}});
  return acc == 1;
}
