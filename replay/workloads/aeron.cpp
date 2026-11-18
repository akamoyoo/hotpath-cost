// real-logic/aeron: the C client's concurrent buffers, the shared-memory paths between a client and the
// media driver: many-to-one and one-to-one ring buffers (write then read one 64-byte message, and
// try_claim + commit + read) and the broadcast buffer (transmit then receive). The buffers are set up
// before measurement; the paths are those of an Aeron client sending commands and receiving events.
#include "util/aeron_platform.h"               // these two switch to C++ headers under C++: outside extern "C"
#include "concurrent/aeron_atomic.h"
extern "C" {
#include "concurrent/aeron_mpsc_rb.h"
#include "concurrent/aeron_spsc_rb.h"
#include "concurrent/aeron_broadcast_transmitter.h"
#include "concurrent/aeron_broadcast_receiver.h"
// aeron_error.c prints through aeron_fprintf, defined in aeronc.c with the whole client; only error
// reporting reaches it, never these paths
int aeron_fprintf(const char *, uint64_t, void *, const char *, ...) { return 0; }
}
#include <cstring>
#include "../drivers/hp_entry.h"
#include "../workload.h"
static size_t seen = 0;
static void on_msg(int32_t, const void *, size_t n, void *) { seen += n; }
static void on_bcast(int32_t, uint8_t *, size_t n, void *) { seen += n; }
namespace hp_entry {
HP_ENTRY size_t mpsc_write_read(aeron_mpsc_rb_t *rb, const void *m, size_t n) {
  aeron_mpsc_rb_write(rb, 1, m, n); return aeron_mpsc_rb_read(rb, on_msg, nullptr, 10);
}
HP_ENTRY size_t mpsc_claim_read(aeron_mpsc_rb_t *rb, const void *m, size_t n) {
  int32_t off = aeron_mpsc_rb_try_claim(rb, 1, n);
  if (off > 0) { std::memcpy(rb->buffer + off, m, n); aeron_mpsc_rb_commit(rb, off); }
  return aeron_mpsc_rb_read(rb, on_msg, nullptr, 10);
}
HP_ENTRY size_t spsc_write_read(aeron_spsc_rb_t *rb, const void *m, size_t n) {
  aeron_spsc_rb_write(rb, 1, m, n); return aeron_spsc_rb_read(rb, on_msg, nullptr, 10);
}
HP_ENTRY int bcast(aeron_broadcast_transmitter_t *t, aeron_broadcast_receiver_t *r, const void *m, size_t n) {
  aeron_broadcast_transmitter_transmit(t, 1, m, n); return aeron_broadcast_receiver_receive(r, on_bcast, nullptr);
}
}
alignas(64) static uint8_t mpsc_buf[65536 + AERON_RB_TRAILER_LENGTH], spsc_buf[65536 + AERON_RB_TRAILER_LENGTH],
    bc_buf[65536 + AERON_BROADCAST_BUFFER_TRAILER_LENGTH];
int main() {
  aeron_mpsc_rb_t mpsc; aeron_spsc_rb_t spsc; aeron_broadcast_transmitter_t tx; aeron_broadcast_receiver_t rx;
  if (aeron_mpsc_rb_init(&mpsc, mpsc_buf, sizeof mpsc_buf) || aeron_spsc_rb_init(&spsc, spsc_buf, sizeof spsc_buf) ||
      aeron_broadcast_transmitter_init(&tx, bc_buf, sizeof bc_buf) || aeron_broadcast_receiver_init(&rx, bc_buf, sizeof bc_buf))
    return 1;
  uint8_t msg[64] = {1, 2, 3};
  run_ops({{"mpsc write+read", [&] { hp_entry::mpsc_write_read(&mpsc, msg, sizeof msg); }},
           {"mpsc try_claim+commit+read", [&] { hp_entry::mpsc_claim_read(&mpsc, msg, sizeof msg); }},
           {"spsc write+read", [&] { hp_entry::spsc_write_read(&spsc, msg, sizeof msg); }},
           {"broadcast transmit+receive", [&] { hp_entry::bcast(&tx, &rx, msg, sizeof msg); }}});
  return seen == 1;
}
