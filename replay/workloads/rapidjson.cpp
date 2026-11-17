// Tencent/rapidjson: DOM parse of a small market-data message, with the default allocator, and with a
// memory-pool allocator on a fixed buffer (the documented way to avoid heap allocation).
#include <rapidjson/document.h>
#include "../drivers/hp_entry.h"
#include "../workload.h"
static const char MSG[] = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double rj_parse(const char *s) { rapidjson::Document d; d.Parse(s); return d["px"].GetDouble(); }
HP_ENTRY double rj_parse_pool(const char *s, char *buf, size_t n, char *pbuf, size_t pn) {
  typedef rapidjson::GenericDocument<rapidjson::UTF8<>, rapidjson::MemoryPoolAllocator<>, rapidjson::MemoryPoolAllocator<>> Doc;
  rapidjson::MemoryPoolAllocator<> va(buf, n), pa(pbuf, pn);
  Doc d(&va, 256, &pa); d.Parse(s); return d["px"].GetDouble();
}
}
int main() {
  static char vb[8192], pb[4096]; double acc = 0;
  run_ops({{"parse + read px", [&] { acc += hp_entry::rj_parse(MSG); }},
           {"parse + read px, pool on a fixed buffer", [&] { acc += hp_entry::rj_parse_pool(MSG, vb, sizeof vb, pb, sizeof pb); }}});
  return acc == 1;
}
