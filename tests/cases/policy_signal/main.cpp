// Signal handlers: which reach functions that are not async-signal-safe?
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
static volatile sig_atomic_t g_flag;
static void good_handler(int) { g_flag = 1; const char m[] = "sig\n"; write(2, m, sizeof m - 1); }
static void log_it(int s) { std::fprintf(stderr, "signal %d\n", s); }        // stdio: not safe
static char* volatile g_buf;
static void bad_handler(int s) { log_it(s); g_buf = (char*)std::malloc(16); std::free(g_buf); }
static void act_handler(int s, siginfo_t*, void*) { g_flag = s; std::memset((void*)&g_flag, 0, 0); }
static void not_a_handler(int s) { std::printf("%d\n", s); }
int main(int argc, char**) {
  std::signal(SIGUSR1, good_handler);
  std::signal(SIGUSR2, bad_handler);
  struct sigaction sa; std::memset(&sa, 0, sizeof sa); sa.sa_sigaction = act_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGTERM, &sa, nullptr);
  if (argc > 5) not_a_handler(argc);
  return 0;
}
