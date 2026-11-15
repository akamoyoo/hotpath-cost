// MengRao/tcpshm (master f825816): "Non-blocking(except client Connect())", "No C++ execptions",
// "No use of mutex or atomic operations", "busy polling for the purpose of low latency".
// Client side of test/echo_client.cc with the same configuration; the callbacks do no I/O.
#include "../../src/tcpshm/tcpshm_client.h"
#include <cstdint>
#include <cstring>
#include "../fx_entry.h"

using namespace tcpshm;

struct CommonConf {                         // test/common.h
  static const uint32_t NameSize = 16;
  static const uint32_t ShmQueueSize = 1024 * 1024;
  static const bool ToLittleEndian = true;
  using LoginUserData = char;
  using LoginRspUserData = char;
};
struct ClientConf : public CommonConf {     // test/echo_client.cc
  static const int64_t NanoInSecond = 1000000000LL;
  static const uint32_t TcpQueueSize = 2000;
  static const uint32_t TcpRecvBufInitSize = 1000;
  static const uint32_t TcpRecvBufMaxSize = 2000;
  static const bool TcpNoDelay = true;
  static const int64_t ConnectionTimeout = 10 * NanoInSecond;
  static const int64_t HeartBeatInverval = 3 * NanoInSecond;
  using ConnectionUserData = char;
};

class Client;
using TSClient = TcpShmClient<Client, ClientConf>;
volatile int64_t g_sink;

class Client : public TSClient {
public:
  Client(const std::string &dir, const std::string &name) : TSClient(dir, name) {}
  void poll_tcp(int64_t now) { PollTcp(now); }
  void poll_shm() { PollShm(); }
  Connection &conn() { return GetConnection(); }
private:
  friend TSClient;
  void OnSystemError(const char *, int e) { g_sink = e; }
  void OnLoginReject(const LoginRspMsg *) { g_sink = 1; }
  int64_t OnLoginSuccess(const LoginRspMsg *) { return 0; }
  void OnSeqNumberMismatch(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { g_sink = 2; }
  void OnServerMsg(MsgHeader *h) { g_sink = h->size; GetConnection().Pop(); }
  void OnDisconnected(const char *, int e) { g_sink = e; }
};
using Conn = TSClient::Connection;

namespace fx_entry {
FX_ENTRY void tcpshm_poll_tcp(Client &c, int64_t now) { c.poll_tcp(now); }
FX_ENTRY void tcpshm_poll_shm(Client &c) { c.poll_shm(); }
// the send pattern of echo_client.cc TrySendMsg: Alloc, fill, Push
FX_ENTRY bool tcpshm_send(Conn &c, uint16_t type, const int *v, int n) {
  MsgHeader *h = c.Alloc(sizeof(int) * n);
  if (!h) return false;
  h->msg_type = type;
  std::memcpy(h + 1, v, sizeof(int) * n);
  c.Push();
  return true;
}
}

int main(int argc, char **argv) {
  Client c("/tmp/x", "c");
  int v[4] = {1, 2, 3, 4};
  fx_entry::tcpshm_poll_tcp(c, argc); fx_entry::tcpshm_poll_shm(c);
  return fx_entry::tcpshm_send(c.conn(), 1, v, 4);
}
