#include "shapes.hpp"
struct Square : Shape { long s = 2; long area() const override { return s * s; } };
Shape* make_square();
Shape* make_circle();
Shape* make_shape(int k) { return k ? make_circle() : make_square(); }
Shape* make_square() { return new Square; }
