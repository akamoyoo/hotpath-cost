// ibireme/yyjson (src/yyjson.c compiled into this file): parse a small market-data message and read a
// field, with the default allocator, and with a fixed buffer through yyjson's pool allocator.
#include "yyjson.c"
#include <cstring>
#include "../drivers/hp_entry.h"
#include "../workload.h"
static const char MSG[] = R"({"type":"trade","sym":"AAPL","px":187.25,"qty":100,"ts":1700000000123456,"cond":["@","F"]})";
namespace hp_entry {
HP_ENTRY double yy_parse(const char *s, size_t n) {
  yyjson_doc *d = yyjson_read(s, n, 0); double px = yyjson_get_real(yyjson_obj_get(yyjson_doc_get_root(d), "px")); yyjson_doc_free(d); return px;
}
HP_ENTRY double yy_parse_pool(const char *s, size_t n, const yyjson_alc *alc) {
  yyjson_doc *d = yyjson_read_opts(const_cast<char *>(s), n, 0, alc, nullptr);
  double px = yyjson_get_real(yyjson_obj_get(yyjson_doc_get_root(d), "px")); yyjson_doc_free(d); return px;
}
}
int main() {
  static char pool[4096]; yyjson_alc alc; yyjson_alc_pool_init(&alc, pool, sizeof pool);
  size_t n = std::strlen(MSG); double acc = 0;
  run_ops({{"parse + read px", [&] { acc += hp_entry::yy_parse(MSG, n); }},
           {"parse + read px, pool allocator", [&] { acc += hp_entry::yy_parse_pool(MSG, n, &alc); }}});
  return acc == 1;
}
