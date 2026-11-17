// msgpack/msgpack-c (C++, branch cpp_master): packing a small order struct into a reused buffer, and
// unpacking it, into a new zone per message (msgpack::unpack returning an object_handle) and into a
// reused zone. Built without Boost (MSGPACK_NO_BOOST).
#include <msgpack.hpp>
#include <string>
#include "../drivers/hp_entry.h"
#include "../workload.h"
struct Order { std::string sym; int64_t px; int32_t qty; int8_t side; MSGPACK_DEFINE(sym, px, qty, side); };
namespace hp_entry {
HP_ENTRY size_t mp_pack(msgpack::sbuffer &buf, const Order &o) { buf.clear(); msgpack::pack(buf, o); return buf.size(); }
HP_ENTRY int64_t mp_unpack(const msgpack::sbuffer &buf) {
  msgpack::object_handle h = msgpack::unpack(buf.data(), buf.size());
  return h.get().via.array.ptr[1].as<int64_t>();
}
HP_ENTRY int64_t mp_unpack_zone(msgpack::zone &z, const msgpack::sbuffer &buf) {
  z.clear(); msgpack::object o = msgpack::unpack(z, buf.data(), buf.size());
  return o.via.array.ptr[1].as<int64_t>();
}
}
int main() {
  msgpack::sbuffer b(256), r(256); Order o{"AAPL", 1870000, 100, 1}; msgpack::pack(r, o); msgpack::zone z; int64_t acc = 0;
  run_ops({{"pack, reused buffer", [&] { acc += hp_entry::mp_pack(b, o); }},
           {"unpack, new zone", [&] { acc += hp_entry::mp_unpack(r); }},
           {"unpack, reused zone", [&] { acc += hp_entry::mp_unpack_zone(z, r); }}});
  return acc == 1;
}
