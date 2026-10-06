// The math module (devices/math.ent): what the machine's math library
// does. Compiled with the program, against the declarations generated for
// it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <math.h>

float ent_math_sqrt(float x) { return sqrtf(x); }
float ent_math_sin(float x) { return sinf(x); }
float ent_math_cos(float x) { return cosf(x); }
float ent_math_tan(float x) { return tanf(x); }
float ent_math_asin(float x) { return asinf(x); }
float ent_math_acos(float x) { return acosf(x); }
float ent_math_atan(float x) { return atanf(x); }
float ent_math_exp(float x) { return expf(x); }
float ent_math_log(float x) { return logf(x); }
float ent_math_log2(float x) { return log2f(x); }
float ent_math_floor(float x) { return floorf(x); }
float ent_math_ceil(float x) { return ceilf(x); }
float ent_math_round(float x) { return roundf(x); }
float ent_math_trunc(float x) { return truncf(x); }
float ent_math_atan2(float y, float x) { return atan2f(y, x); }
float ent_math_pow(float x, float y) { return powf(x, y); }
float ent_math_fmod(float x, float y) { return fmodf(x, y); }

double ent_math_sqrt64(double x) { return sqrt(x); }
double ent_math_sin64(double x) { return sin(x); }
double ent_math_cos64(double x) { return cos(x); }
double ent_math_tan64(double x) { return tan(x); }
double ent_math_asin64(double x) { return asin(x); }
double ent_math_acos64(double x) { return acos(x); }
double ent_math_atan64(double x) { return atan(x); }
double ent_math_exp64(double x) { return exp(x); }
double ent_math_log64(double x) { return log(x); }
double ent_math_log264(double x) { return log2(x); }
double ent_math_floor64(double x) { return floor(x); }
double ent_math_ceil64(double x) { return ceil(x); }
double ent_math_round64(double x) { return round(x); }
double ent_math_trunc64(double x) { return trunc(x); }
double ent_math_atan264(double y, double x) { return atan2(y, x); }
double ent_math_pow64(double x, double y) { return pow(x, y); }
double ent_math_fmod64(double x, double y) { return fmod(x, y); }
