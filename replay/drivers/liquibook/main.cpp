// enewhuis/liquibook 2.0.0: the order book operations (add, cancel, replace) of the
// SimpleOrderBook used by the project's own tests and performance test (src/main.cpp).
#include <simple/simple_order_book.h>
#include <simple/simple_order.h>
#include "../hp_entry.h"

using Book = liquibook::simple::SimpleOrderBook<5>;
using liquibook::simple::SimpleOrder;

namespace hp_entry {
HP_ENTRY bool lb_add(Book &b, SimpleOrder *o) { return b.add(o); }
HP_ENTRY bool lb_add_ioc(Book &b, SimpleOrder *o) { return b.add(o, liquibook::book::OrderCondition::oc_immediate_or_cancel); }
HP_ENTRY void lb_cancel(Book &b, SimpleOrder *o) { b.cancel(o); }
HP_ENTRY bool lb_replace(Book &b, SimpleOrder *o, int32_t dq, liquibook::book::Price p) { return b.replace(o, dq, p); }
HP_ENTRY bool lb_add_cond(Book &b, SimpleOrder *o, liquibook::book::OrderConditions c) { return b.add(o, c); }
}

int main(int argc, char **) {
  Book b;
  SimpleOrder o(true, 100, 10), p(false, 100, 10);
  hp_entry::lb_add(b, &o); hp_entry::lb_add_ioc(b, &p); hp_entry::lb_replace(b, &o, argc, 101);
  return 0;
}
