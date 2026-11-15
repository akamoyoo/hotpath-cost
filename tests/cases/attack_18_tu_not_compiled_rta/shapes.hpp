struct Base { virtual long f(long) = 0; virtual ~Base(); };
struct Quiet : Base { long f(long x) override; };
struct Loud : Base { long f(long x) override; };
Base* make_loud(long);
