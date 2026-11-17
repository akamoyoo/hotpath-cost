struct Sink { virtual void on(long x) = 0; virtual ~Sink() {} };
Sink* make_quiet();
struct Src { virtual long get(long x) = 0; virtual ~Src() {} };
Src* make_loud();
