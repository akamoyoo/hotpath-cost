int* volatile g_last;
#ifndef NO_ALLOC
void log_event(int x) { g_last = new int(x); }   // the injected change: one allocation
#else
void log_event(int x) { (void)x; }
#endif
int helper(int x) { log_event(x); return x * 2; }
