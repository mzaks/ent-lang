// The math module (devices/math.ent): what of the machine's math library
// math.ent does not write itself. Compiled with the program, against the
// declarations generated for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <math.h>

float ent_math_asin(float x) { return asinf(x); }
float ent_math_acos(float x) { return acosf(x); }

double ent_math_asin64(double x) { return asin(x); }
double ent_math_acos64(double x) { return acos(x); }
