// Host program for examples/integrate.mlir. It owns the world's storage,
// fills it, runs a few frames through the lowered `@frame` schedule and
// prints every position.
//
// The argument order follows the world ABI of --ecs-lower-to-loops: per
// archetype in declaration order, the entity count, then one column per
// field in component order and field order.

#include <stdint.h>
#include <stdio.h>

// A rank-1 memref descriptor as passed to `_mlir_ciface_*` functions.
typedef struct {
  float *allocated;
  float *aligned;
  int64_t offset;
  int64_t size;
  int64_t stride;
} Column;

static Column column(float *data, int64_t size) {
  return (Column){data, data, 0, size, 1};
}

void _mlir_ciface_frame(float dt,
                        // @Body: Position, Velocity, Mass
                        int64_t bodies, Column *bx, Column *by, Column *bdx,
                        Column *bdy, Column *bkg,
                        // @Particle: Position, Velocity, Lifetime
                        int64_t particles, Column *px, Column *py,
                        Column *pdx, Column *pdy, Column *plife,
                        // @Scenery: Position
                        int64_t scenery, Column *sx, Column *sy);

enum { BODIES = 2, PARTICLES = 2, SCENERY = 1, FRAMES = 3 };

int main(void) {
  float bodyX[BODIES] = {0, 10}, bodyY[BODIES] = {100, 50};
  float bodyDx[BODIES] = {1, -2}, bodyDy[BODIES] = {0, 4};
  float bodyKg[BODIES] = {1, 5};
  float partX[PARTICLES] = {0, 3}, partY[PARTICLES] = {0, 3};
  float partDx[PARTICLES] = {2, 0}, partDy[PARTICLES] = {1, -1};
  float partLife[PARTICLES] = {10, 2};
  float sceneX[SCENERY] = {7}, sceneY[SCENERY] = {7};

  Column bx = column(bodyX, BODIES), by = column(bodyY, BODIES);
  Column bdx = column(bodyDx, BODIES), bdy = column(bodyDy, BODIES);
  Column bkg = column(bodyKg, BODIES);
  Column px = column(partX, PARTICLES), py = column(partY, PARTICLES);
  Column pdx = column(partDx, PARTICLES), pdy = column(partDy, PARTICLES);
  Column plife = column(partLife, PARTICLES);
  Column sx = column(sceneX, SCENERY), sy = column(sceneY, SCENERY);

  for (int frame = 0; frame < FRAMES; ++frame)
    _mlir_ciface_frame(0.5f, BODIES, &bx, &by, &bdx, &bdy, &bkg, PARTICLES,
                       &px, &py, &pdx, &pdy, &plife, SCENERY, &sx, &sy);

  for (int i = 0; i < BODIES; ++i)
    printf("body %d: pos (%.4f, %.4f) vel (%.4f, %.4f)\n", i, bodyX[i],
           bodyY[i], bodyDx[i], bodyDy[i]);
  for (int i = 0; i < PARTICLES; ++i)
    printf("particle %d: pos (%.4f, %.4f) vel (%.4f, %.4f) life %.4f\n", i,
           partX[i], partY[i], partDx[i], partDy[i], partLife[i]);
  for (int i = 0; i < SCENERY; ++i)
    printf("scenery %d: pos (%.4f, %.4f)\n", i, sceneX[i], sceneY[i]);
  return 0;
}
