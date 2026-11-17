// USCiLab/cereal: the binary archive of an order message through std::stringstream, the documented use.
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cstdint>
#include <sstream>
#include <string>
#include "../drivers/hp_entry.h"
#include "../bench_common.h"
struct Order { uint64_t id; double px; int32_t qty; std::string sym;
  template <class A> void serialize(A &a) { a(id, px, qty, sym); } };
namespace hp_entry {
HP_ENTRY size_t ce_write(const Order &o) { std::ostringstream s; { cereal::BinaryOutputArchive a(s); a(o); } return s.str().size(); }
HP_ENTRY uint64_t ce_read(const std::string &bytes) { std::istringstream s(bytes); cereal::BinaryInputArchive a(s); Order o; a(o); return o.id; }
}
int main() {
  Order o{42, 187.25, 100, "AAPL"}; std::ostringstream s; { cereal::BinaryOutputArchive a(s); a(o); } std::string bytes = s.str();
  size_t acc = 0;
  run_ops({{"serialize to ostringstream", [&] { acc += hp_entry::ce_write(o); }},
           {"deserialize from istringstream", [&] { acc += hp_entry::ce_read(bytes); }}}, 1000, 50000);
  return acc == 1;
}
