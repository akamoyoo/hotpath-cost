// google/flatbuffers: building and reading a small order message with the schema-less builder API (the
// calls generated code makes), with a builder reused across messages (Clear()) and with a new builder
// per message, and reading two fields back.
#include <flatbuffers/flatbuffers.h>
#include "../drivers/hp_entry.h"
#include "../workload.h"
namespace hp_entry {
static void build(flatbuffers::FlatBufferBuilder &b, int i) {
  auto sym = b.CreateString("AAPL");
  auto start = b.StartTable();
  b.AddOffset(4, sym);                    // symbol
  b.AddElement<int64_t>(6, 1870000 + i, 0);  // price
  b.AddElement<int32_t>(8, 100, 0);       // quantity
  b.AddElement<int8_t>(10, 1, 0);         // side
  b.Finish(flatbuffers::Offset<flatbuffers::Table>(b.EndTable(start)));
}
HP_ENTRY size_t fb_build_reused(flatbuffers::FlatBufferBuilder &b, int i) { b.Clear(); build(b, i); return b.GetSize(); }
HP_ENTRY size_t fb_build_new(int i) { flatbuffers::FlatBufferBuilder b(256); build(b, i); return b.GetSize(); }
HP_ENTRY int64_t fb_read(const uint8_t *buf) {
  auto t = flatbuffers::GetRoot<flatbuffers::Table>(buf);
  return t->GetField<int64_t>(6, 0) + t->GetField<int32_t>(8, 0);
}
}
int main() {
  flatbuffers::FlatBufferBuilder b(256), r(256); hp_entry::build(r, 0); int i = 0; int64_t acc = 0;
  run_ops({{"build, reused builder", [&] { acc += hp_entry::fb_build_reused(b, i++); }},
           {"build, new builder", [&] { acc += hp_entry::fb_build_new(i++); }},
           {"read two fields", [&] { acc += hp_entry::fb_read(r.GetBufferPointer()); }}});
  return acc == 1;
}
