#include <cstdlib>
#define NI __attribute__((noinline, used))
void* make(long); void release(void*); void* make_zero(long); void* make_aligned(long); void* grow(void*, long);
long* make_obj(long); void drop_obj(long*); long* make_arr(long); void drop_arr(long*); void* nothing(); long* nothing_obj();
long sum3(long); long len_of(const char*); void* make_nt(long); int thrower(long); void lock_unlock(); long wr0(int);
NI long e_malloc_free(long x) { void* p = make(x + 8); release(p); return x; }            // always alloc, always free
NI long e_malloc_unused(long x) { make(64); return x; }                                    // always alloc (a leak the optimiser may drop)
NI long e_calloc_free(long x) { void* p = make_zero(32); release(p); return x; }
NI long e_aligned_free(long x) { void* p = make_aligned(128); release(p); return x; }
NI long e_realloc_null(long x) { void* p = grow(nullptr, 64); release(p); return x; }
NI long e_new_delete(long x) { long* p = make_obj(x); long v = *p; drop_obj(p); return v; }
NI long e_newarr_delete(long x) { long* p = make_arr(x); drop_arr(p); return x; }
NI long e_free_null(long x) { release(nothing()); return x; }                              // always free
NI long e_delete_null(long x) { drop_obj(nothing_obj()); return x; }                       // always free?
NI long e_vector_temp(long x) { return sum3(x); }
NI long e_string_temp(long x) { return len_of("a string that is longer than the small buffer of std::string"); }
NI long e_new_nothrow(long x) { void* p = make_nt(x); release(nullptr); ::operator delete(p); return x; }
NI long e_throw_catch(long x) { try { thrower(x); } catch (long v) { return v; } return 0; }   // always throw
NI long e_lock(long x) { lock_unlock(); return x; }                                           // always lock
NI long e_write0(long x) { return wr0(1) + x; }                                               // always syscall
typedef long (*F)(long);
F volatile tab[] = {e_malloc_free, e_malloc_unused, e_calloc_free, e_aligned_free, e_realloc_null, e_new_delete, e_newarr_delete, e_free_null,
                    e_delete_null, e_vector_temp, e_string_temp, e_new_nothrow, e_throw_catch, e_lock, e_write0};
int main(int argc, char**) { long a = argc; volatile long s = 0; for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) { F f = tab[i]; s = s + f(a); } return 0; }
