struct Sink { virtual long on(long x) = 0; virtual ~Sink() {} };
Sink* make_from_library(int kind);          // defined in libplugin (no bitcode): returns an object of a class derived there
