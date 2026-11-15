// shared_ptr copies: atomic reference counting; the last release frees.
#include "../../fx_test.h"
#include <memory>
#include <thread>
struct X { long v = 7; };
FX_NOINLINE long use(const std::shared_ptr<X>& p) FX_NB { std::shared_ptr<X> q = p; return q->v + q.use_count(); }
int main() {
  auto p = std::make_shared<X>();
  std::thread t([&] { (void)use(p); });   // makes the program multi-threaded, so the atomic path is taken
  long r = 0;
  for (int i = 0; i < 100; i++) r += use(p);
  t.join();
  return (int)(r & 1);
}
