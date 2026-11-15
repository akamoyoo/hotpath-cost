// A small example for the README: five functions, none of them annotated.
#include <mutex>
#include <vector>

struct Order { long id, px, qty; };

static std::vector<Order> book;
static std::mutex mu;

#define KEEP __attribute__((noinline))   // only so that each function stays visible in the output

KEEP long checksum(const Order &o) { return o.id * 31 + o.px * 7 + o.qty; }

KEEP void append(const Order &o) { book.push_back(o); }          // allocates when the vector grows

KEEP Order *make(long id) { return new Order{id, 0, 0}; }        // allocates on every call

KEEP long insert_locked(const Order &o) {                        // locks on every call
  std::lock_guard<std::mutex> g(mu);
  append(o);
  return checksum(o);
}

KEEP long route(const Order &o, bool slow) { return slow ? insert_locked(o) : checksum(o); }

int main(int argc, char **) {
  Order *o = make(argc);
  long r = route(*o, argc > 1);
  delete o;
  return (int)r;
}
