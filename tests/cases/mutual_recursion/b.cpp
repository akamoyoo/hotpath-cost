bool is_even(unsigned n);
bool is_odd(unsigned n) { return n == 0 ? false : is_even(n - 1); }
