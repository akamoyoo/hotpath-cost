// fraillt/bitsery: binary serialization of an order message into a fixed buffer, and back.
#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/array.h>
#include <array>
#include <cstdint>
#include "../drivers/hp_entry.h"
#include "../workload.h"
struct Order { uint64_t id; double px; int32_t qty; std::array<char, 8> sym; };
template <typename S> void serialize(S &s, Order &o) { s.value8b(o.id); s.value8b(o.px); s.value4b(o.qty); s.container1b(o.sym); }
using Buf = std::array<uint8_t, 64>;
namespace hp_entry {
HP_ENTRY size_t bs_write(Buf &b, Order &o) { return bitsery::quickSerialization<bitsery::OutputBufferAdapter<Buf>>(b, o); }
HP_ENTRY bool bs_read(const Buf &b, size_t n, Order &o) { return bitsery::quickDeserialization<bitsery::InputBufferAdapter<Buf>>({b.begin(), n}, o).second; }
}
int main() {
  Buf b{}; Order o{42, 187.25, 100, {'A', 'A', 'P', 'L'}}, r{}; size_t n = 0, ok = 0;
  run_ops({{"serialize order", [&] { n = hp_entry::bs_write(b, o); }},
           {"deserialize order", [&] { ok += hp_entry::bs_read(b, n, r); }}});
  return ok == 1;
}
