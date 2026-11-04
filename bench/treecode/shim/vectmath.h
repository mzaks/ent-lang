/* Vectors and 3 by 3 matrices for treecode (bench/treecode/shim), written
   for ent-lang. treecode calls them by the names NEMO's vectmath.h gives
   them, so those names are kept, each a call of a function below; the
   functions are ent-lang's own. A scalar result is the first argument,
   then what is written, then what is read, as treecode passes them. */
#ifndef SHIM_VECTMATH_H
#define SHIM_VECTMATH_H
#include "stdinc.h"
#define NDIM 3
typedef real vector[NDIM];
typedef real matrix[NDIM][NDIM];

typedef const real *vec_in;
typedef real (*mat_out)[NDIM];
typedef const real (*mat_in)[NDIM];

/* Vectors. */
static inline void vec_zero(real *out) {
  for (int k = 0; k < NDIM; k++)
    out[k] = 0.0;
}
static inline void vec_copy(real *out, vec_in a) {
  for (int k = 0; k < NDIM; k++)
    out[k] = a[k];
}
static inline void vec_add(real *out, vec_in a, vec_in b) {
  for (int k = 0; k < NDIM; k++)
    out[k] = a[k] + b[k];
}
static inline void vec_sub(real *out, vec_in a, vec_in b) {
  for (int k = 0; k < NDIM; k++)
    out[k] = a[k] - b[k];
}
static inline void vec_scale(real *out, vec_in a, real f) {
  for (int k = 0; k < NDIM; k++)
    out[k] = a[k] * f;
}
static inline void vec_divide(real *out, vec_in a, real f) {
  for (int k = 0; k < NDIM; k++)
    out[k] = a[k] / f;
}
/* out += a f */
static inline void vec_add_scaled(real *out, vec_in a, real f) {
  for (int k = 0; k < NDIM; k++)
    out[k] += a[k] * f;
}
/* out += a f + b g */
static inline void vec_add_scaled2(real *out, vec_in a, real f, vec_in b,
                                   real g) {
  for (int k = 0; k < NDIM; k++)
    out[k] += a[k] * f + b[k] * g;
}
static inline real vec_dot(vec_in a, vec_in b) {
  real sum = 0.0;
  for (int k = 0; k < NDIM; k++)
    sum += a[k] * b[k];
  return sum;
}
static inline real vec_length(vec_in a) { return sqrt(vec_dot(a, a)); }
static inline real vec_distance(vec_in a, vec_in b) {
  real sum = 0.0;
  for (int k = 0; k < NDIM; k++) {
    real d = a[k] - b[k];
    sum += d * d;
  }
  return sqrt(sum);
}
/* out = a - b, and how long out is, squared. */
static inline real vec_sub_square(real *out, vec_in a, vec_in b) {
  real sum = 0.0;
  for (int k = 0; k < NDIM; k++) {
    out[k] = a[k] - b[k];
    sum += out[k] * out[k];
  }
  return sum;
}
static inline void vec_cross(real *out, vec_in a, vec_in b) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Matrices, a row at a time. */
static inline void mat_zero(mat_out out) {
  for (int r = 0; r < NDIM; r++)
    vec_zero(out[r]);
}
static inline void mat_copy(mat_out out, mat_in a) {
  for (int r = 0; r < NDIM; r++)
    vec_copy(out[r], a[r]);
}
static inline void mat_identity(mat_out out) {
  for (int r = 0; r < NDIM; r++)
    for (int c = 0; c < NDIM; c++)
      out[r][c] = r == c ? 1.0 : 0.0;
}
static inline void mat_add(mat_out out, mat_in a, mat_in b) {
  for (int r = 0; r < NDIM; r++)
    vec_add(out[r], a[r], b[r]);
}
static inline void mat_sub(mat_out out, mat_in a, mat_in b) {
  for (int r = 0; r < NDIM; r++)
    vec_sub(out[r], a[r], b[r]);
}
static inline void mat_scale(mat_out out, mat_in a, real f) {
  for (int r = 0; r < NDIM; r++)
    vec_scale(out[r], a[r], f);
}
/* out = a b^T */
static inline void mat_outer(mat_out out, vec_in a, vec_in b) {
  for (int r = 0; r < NDIM; r++)
    vec_scale(out[r], b, a[r]);
}
/* out = m a, and out . a */
static inline real mat_apply_dot(real *out, mat_in m, vec_in a) {
  real sum = 0.0;
  for (int r = 0; r < NDIM; r++) {
    out[r] = vec_dot(m[r], a);
    sum += out[r] * a[r];
  }
  return sum;
}

#define CLRV(v) vec_zero(v)
#define SETV(v, u) vec_copy(v, u)
#define ADDV(v, u, w) vec_add(v, u, w)
#define SUBV(v, u, w) vec_sub(v, u, w)
#define MULVS(v, u, s) vec_scale(v, u, s)
#define DIVVS(v, u, s) vec_divide(v, u, s)
#define ADDMULVS(v, u, s) vec_add_scaled(v, u, s)
#define ADDMULVS2(v, u, s, w, r) vec_add_scaled2(v, u, s, w, r)
#define DOTVP(s, v, u) ((s) = vec_dot(v, u))
#define ABSV(s, v) ((s) = vec_length(v))
#define DISTV(s, u, v) ((s) = vec_distance(u, v))
#define DOTPSUBV(s, v, u, w) ((s) = vec_sub_square(v, u, w))
#define CROSSVP(v, u, w) vec_cross(v, u, w)
#define CLRM(p) mat_zero(p)
#define SETM(p, q) mat_copy(p, (mat_in)(q))
#define SETMI(p) mat_identity(p)
#define ADDM(p, q, r) mat_add(p, (mat_in)(q), (mat_in)(r))
#define SUBM(p, q, r) mat_sub(p, (mat_in)(q), (mat_in)(r))
#define MULMS(p, q, s) mat_scale(p, (mat_in)(q), s)
#define OUTVP(p, v, u) mat_outer(p, v, u)
#define DOTPMULMV(s, v, p, u) ((s) = mat_apply_dot(v, (mat_in)(p), u))
#endif
