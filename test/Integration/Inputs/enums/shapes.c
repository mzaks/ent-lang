// The C of shapes.ent: an enum is a byte, its cases are named.
#include "ent_extern.h"
int32_t ent_shapes_corners(ent_shapes_Shape shape) { return shape == ent_shapes_Shape_Square ? 4 : 0; }
ent_shapes_Shape ent_shapes_other(ent_shapes_Shape shape) { return shape == ent_shapes_Shape_Round ? ent_shapes_Shape_Square : ent_shapes_Shape_Round; }
