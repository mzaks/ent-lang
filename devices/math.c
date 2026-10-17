// The math module (devices/math.ent): what of the machine's math library
// math.ent does not write itself. Compiled with the program, against the
// declarations generated for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <math.h>

float ent_math_tan(float x) { return tanf(x); }
float ent_math_asin(float x) { return asinf(x); }
float ent_math_acos(float x) { return acosf(x); }
float ent_math_atan(float x) { return atanf(x); }
float ent_math_log2(float x) { return log2f(x); }
float ent_math_atan2(float y, float x) { return atan2f(y, x); }
float ent_math_fmod(float x, float y) { return fmodf(x, y); }

double ent_math_tan64(double x) { return tan(x); }
double ent_math_asin64(double x) { return asin(x); }
double ent_math_acos64(double x) { return acos(x); }
double ent_math_atan64(double x) { return atan(x); }
double ent_math_log264(double x) { return log2(x); }
double ent_math_atan264(double y, double x) { return atan2(y, x); }
double ent_math_fmod64(double x, double y) { return fmod(x, y); }
