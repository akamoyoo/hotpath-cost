#pragma once
#include <functional>
struct Shape { virtual long area() const = 0; virtual ~Shape() = default; };
Shape* make_shape(int k);          // defined in factory.cpp; the allocating override is in circle.cpp
struct Holder { int (*fp)(int); std::function<int(int)> cb; };
void fill_holder(Holder& h);       // defined in hooks.cpp
extern int (*g_hook)(int);         // set by a global constructor in hooks.cpp
