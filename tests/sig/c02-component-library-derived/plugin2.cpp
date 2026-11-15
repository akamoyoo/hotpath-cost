#include <cstdlib>
struct QBase { virtual long get(long x) { return x; } virtual ~QBase() {} };
static void* volatile keep;
struct LibQ final : QBase { long get(long x) override { keep = malloc(x + 8); return x; } };
QBase* make_q_from_library() { static LibQ q; return &q; }
