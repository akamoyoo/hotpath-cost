// nlohmann/json (single header): parse a small market-data message, read a field, and serialize.
#include <nlohmann/json.hpp>
#include <string>
#include "../drivers/hp_entry.h"
#include "../workload.h"
static const char MSG[] = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double js_parse_px(const char *s) { return nlohmann::json::parse(s)["px"].get<double>(); }
HP_ENTRY double js_get_px(const nlohmann::json &j) { return j["px"].get<double>(); }
HP_ENTRY size_t js_dump(const nlohmann::json &j) { return j.dump().size(); }
}
int main() {
  nlohmann::json j = nlohmann::json::parse(MSG); double acc = 0;
  run_ops({{"parse + read px", [&] { acc += hp_entry::js_parse_px(MSG); }},
           {"read px of a parsed object", [&] { acc += hp_entry::js_get_px(j); }},
           {"dump to std::string", [&] { acc += hp_entry::js_dump(j); }}}, 1000, 50000);
  return acc == 1;
}
