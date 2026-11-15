// enewhuis/liquibook 2.0.0: the order book operations (add, cancel, replace) of the
// SimpleOrderBook used by the project's own tests and performance test (src/main.cpp).
#include <simple/simple_order_book.h>
#include <simple/simple_order.h>
#include "../fx_entry.h"

using Book = liquibook::simple::SimpleOrderBook<5>;
using liquibook::simple::SimpleOrder;

namespace fx_entry {
FX_ENTRY bool lb_add(Book &b, SimpleOrder *o) { return b.add(o); }
FX_ENTRY bool lb_add_ioc(Book &b, SimpleOrder *o) { return b.add(o, liquibook::book::OrderCondition::oc_immediate_or_cancel); }
FX_ENTRY void lb_cancel(Book &b, SimpleOrder *o) { b.cancel(o); }
FX_ENTRY bool lb_replace(Book &b, SimpleOrder *o, int32_t dq, liquibook::book::Price p) { return b.replace(o, dq, p); }
}

int main(int argc, char **) {
  Book b;
  SimpleOrder o(true, 100, 10), p(false, 100, 10);
  fx_entry::lb_add(b, &o); fx_entry::lb_add_ioc(b, &p); fx_entry::lb_replace(b, &o, argc, 101);
  return 0;
}
