#include "shapes.hpp"
#include <vector>
std::vector<long>* volatile g_area_log;
struct Circle : Shape { long r = 3; long area() const override { g_area_log = new std::vector<long>(1, r); return 3 * r * r; } };
Shape* make_circle() { return new Circle; }
