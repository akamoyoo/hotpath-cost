// stephenberry/glaze: read a small market-data message into a reused struct, and write it back to a
// reused string.
#include <glaze/glaze.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "../drivers/hp_entry.h"
#include "../workload.h"
struct Trade { std::string type, sym; double px{}; int qty{}; int64_t ts{}; std::vector<std::string> cond; };
static std::string MSG = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double gz_read(Trade &t, const std::string &s) { auto e = glz::read_json(t, s); return e ? -1 : t.px; }
HP_ENTRY size_t gz_write(const Trade &t, std::string &out) { out.clear(); auto e = glz::write_json(t, out); return e ? 0 : out.size(); }
}
int main() {
  Trade t; std::string out; out.reserve(256); double acc = 0;
  run_ops({{"read into a reused struct", [&] { acc += hp_entry::gz_read(t, MSG); }},
           {"write to a reused string", [&] { acc += double(hp_entry::gz_write(t, out)); }}});
  return acc == 1;
}
