int on_tick(int); int on_order(int); int on_packet(const unsigned char*, int);
int main(int argc, char**) { unsigned char b[4] = {1, 2, 3, 4}; return on_tick(argc) + on_order(argc) + on_packet(b, 4); }
