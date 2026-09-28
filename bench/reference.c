// Hand-written reference for examples/integrate.mlir: the fused frame as a
// C programmer would write it, with the same signature as the lowered
// schedule. Compile with -ffp-contract=off so the arithmetic matches the
// generated code exactly (no fused multiply-add). Define RESTRICT=restrict
// to tell the compiler that columns do not alias.

#include <stdint.h>

#ifndef RESTRICT
#define RESTRICT
#endif

typedef struct {
  float *allocated;
  float *aligned;
  int64_t offset;
  int64_t size;
  int64_t stride;
} Column;

static void bodies(float dt, float g, int64_t n, float *RESTRICT x,
                   float *RESTRICT y, float *RESTRICT dx, float *RESTRICT dy) {
  for (int64_t i = 0; i < n; ++i) {
    dy[i] = dy[i] - g * dt;
    x[i] = x[i] + dx[i] * dt;
    y[i] = y[i] + dy[i] * dt;
  }
}

static void particles(float dt, float w, int64_t n, float *RESTRICT x,
                      float *RESTRICT y, float *RESTRICT dx,
                      float *RESTRICT dy, float *RESTRICT life) {
  for (int64_t i = 0; i < n; ++i) {
    dx[i] = dx[i] + w * dt;
    x[i] = x[i] + dx[i] * dt;
    y[i] = y[i] + dy[i] * dt;
    life[i] = life[i] - dt;
  }
}

void _mlir_ciface_frame(float dt, int64_t nb, Column *bx, Column *by,
                        Column *bdx, Column *bdy, Column *bkg, int64_t np,
                        Column *px, Column *py, Column *pdx, Column *pdy,
                        Column *plife, int64_t ns, Column *sx, Column *sy) {
  (void)bkg, (void)ns, (void)sx, (void)sy;
  bodies(dt, 9.81f, nb, bx->aligned, by->aligned, bdx->aligned, bdy->aligned);
  particles(dt, 2.0f, np, px->aligned, py->aligned, pdx->aligned, pdy->aligned,
            plife->aligned);
}
