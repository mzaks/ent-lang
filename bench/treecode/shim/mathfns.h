/* What treecode takes from NEMO's mathfns.h (bench/treecode/shim). */
#ifndef SHIM_MATHFNS_H
#define SHIM_MATHFNS_H
#include "stdinc.h"
#define rsqrt sqrt
#define rabs fabs
#define rpow pow
#define rsqr(x) ((x) * (x))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
double xrandom(double low, double high);
void pickshell(real *vec, int ndim, real radius);
#endif
