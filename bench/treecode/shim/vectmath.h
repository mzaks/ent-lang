/* What treecode takes from NEMO's vectmath.h (bench/treecode/shim): three
   dimensions, the vector and matrix operations it uses, as NEMO's. */
#ifndef SHIM_VECTMATH_H
#define SHIM_VECTMATH_H
#define NDIM 3
typedef real vector[NDIM];
typedef real matrix[NDIM][NDIM];

#define CLRV(v)                                                                \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = 0.0;                                                           \
  }
#define SETV(v, u)                                                             \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = (u)[_i];                                                       \
  }
#define ADDV(v, u, w)                                                          \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = (u)[_i] + (w)[_i];                                             \
  }
#define SUBV(v, u, w)                                                          \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = (u)[_i] - (w)[_i];                                             \
  }
#define MULVS(v, u, s)                                                         \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = (u)[_i] * (s);                                                 \
  }
#define DIVVS(v, u, s)                                                         \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] = (u)[_i] / (s);                                                 \
  }
#define ADDMULVS(v, u, s)                                                      \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] += (u)[_i] * (s);                                                \
  }
#define ADDMULVS2(v, u, s, w, r)                                               \
  {                                                                            \
    int _i;                                                                    \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (v)[_i] += (u)[_i] * (s) + (w)[_i] * (r);                                \
  }
#define DOTVP(s, v, u)                                                         \
  {                                                                            \
    int _i;                                                                    \
    (s) = 0.0;                                                                 \
    for (_i = 0; _i < NDIM; _i++)                                              \
      (s) += (v)[_i] * (u)[_i];                                                \
  }
#define ABSV(s, v)                                                             \
  {                                                                            \
    real _t;                                                                   \
    int _i;                                                                    \
    _t = 0.0;                                                                  \
    for (_i = 0; _i < NDIM; _i++)                                              \
      _t += (v)[_i] * (v)[_i];                                                 \
    (s) = sqrt(_t);                                                            \
  }
#define DISTV(s, u, v)                                                         \
  {                                                                            \
    real _t, _d;                                                               \
    int _i;                                                                    \
    _t = 0.0;                                                                  \
    for (_i = 0; _i < NDIM; _i++) {                                            \
      _d = (u)[_i] - (v)[_i];                                                  \
      _t += _d * _d;                                                           \
    }                                                                          \
    (s) = sqrt(_t);                                                            \
  }
#define DOTPSUBV(s, v, u, w)                                                   \
  {                                                                            \
    int _i;                                                                    \
    (s) = 0.0;                                                                 \
    for (_i = 0; _i < NDIM; _i++) {                                            \
      (v)[_i] = (u)[_i] - (w)[_i];                                             \
      (s) += (v)[_i] * (v)[_i];                                                \
    }                                                                          \
  }
#define CROSSVP(v, u, w)                                                       \
  {                                                                            \
    (v)[0] = (u)[1] * (w)[2] - (u)[2] * (w)[1];                                \
    (v)[1] = (u)[2] * (w)[0] - (u)[0] * (w)[2];                                \
    (v)[2] = (u)[0] * (w)[1] - (u)[1] * (w)[0];                                \
  }

#define CLRM(p)                                                                \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = 0.0;                                                     \
  }
#define SETM(p, q)                                                             \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (q)[_i][_j];                                             \
  }
#define SETMI(p)                                                               \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (_i == _j) ? 1.0 : 0.0;                                  \
  }
#define ADDM(p, q, r)                                                          \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (q)[_i][_j] + (r)[_i][_j];                               \
  }
#define SUBM(p, q, r)                                                          \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (q)[_i][_j] - (r)[_i][_j];                               \
  }
#define MULMS(p, q, s)                                                         \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (q)[_i][_j] * (s);                                       \
  }
#define OUTVP(p, v, u)                                                         \
  {                                                                            \
    int _i, _j;                                                                \
    for (_i = 0; _i < NDIM; _i++)                                              \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (p)[_i][_j] = (v)[_i] * (u)[_j];                                       \
  }
#define DOTPMULMV(s, v, p, u)                                                  \
  {                                                                            \
    int _i, _j;                                                                \
    (s) = 0.0;                                                                 \
    for (_i = 0; _i < NDIM; _i++) {                                            \
      (v)[_i] = 0.0;                                                           \
      for (_j = 0; _j < NDIM; _j++)                                            \
        (v)[_i] += (p)[_i][_j] * (u)[_j];                                      \
      (s) += (v)[_i] * (u)[_i];                                                \
    }                                                                          \
  }
#endif
