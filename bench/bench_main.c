// Benchmark host for examples/integrate.mlir: runs the lowered `frame`
// schedule (or a hand-written reference with the same signature) over N
// entities per archetype and reports the best time per frame.
//
// Usage: bench <entities per archetype> <repetitions> <target ms>
// Prints: n=<N> frames=<F> ns_per_frame=<best> checksum=<sum of all columns>
//
// The checksum comes from a fixed validation run of VALIDATION_FRAMES
// frames on fresh columns, so it is comparable across variants: fusion and
// parallel loops keep the order of floating point operations per entity.
// Timing then continues on the same columns with a frame count calibrated
// so that one repetition takes about <target ms>.

#include "integrate_world.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// The initial values must not depend on build flags: without this, clang
// contracts `base + step * i` into a fused multiply-add in some builds and
// not in others, and the checksums of equivalent variants differ.
#pragma clang fp contract(off)

enum { NUM_COLUMNS = 12 };

static void fill(float *column, int64_t n, float base, float step) {
  for (int64_t i = 0; i < n; ++i)
    column[i] = base + step * (float)(i % 1024);
}

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

enum { VALIDATION_FRAMES = 10 };

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <entities> <repetitions> <target ms>\n",
            argv[0]);
    return 2;
  }
  int64_t n = atoll(argv[1]);
  int reps = atoi(argv[2]);
  uint64_t target = (uint64_t)atoll(argv[3]) * 1000000;

  ecs_world *w = ecs_world_create();
  if (!w || !ecs_Body_spawn_n(w, n) || !ecs_Particle_spawn_n(w, n) ||
      !ecs_Scenery_spawn_n(w, n) || !ecs_Player_spawn_n(w, 1)) {
    fprintf(stderr, "n=%lld does not fit the world\n", (long long)n);
    return 1;
  }
  float *c[NUM_COLUMNS] = {
      ecs_Body_Position_x(w),        ecs_Body_Position_y(w),
      ecs_Body_Velocity_dx(w),       ecs_Body_Velocity_dy(w),
      ecs_Body_Mass_kg(w),           ecs_Particle_Position_x(w),
      ecs_Particle_Position_y(w),    ecs_Particle_Velocity_dx(w),
      ecs_Particle_Velocity_dy(w),   ecs_Particle_Lifetime_seconds(w),
      ecs_Scenery_Position_x(w),     ecs_Scenery_Position_y(w),
  };
  static const float base[NUM_COLUMNS] = {0, 100, 1, 0, 1, 0,
                                          0, 2,   1, 10, 7, 7};
  static const float step[NUM_COLUMNS] = {0.5f,  0.25f, 0.01f, 0.02f,
                                          0.1f,  0.5f,  0.25f, -0.01f,
                                          0.02f, 0.1f,  0.5f,  0.25f};
  for (int k = 0; k < NUM_COLUMNS; ++k)
    fill(c[k], n, base[k], step[k]);
  *ecs_Wind_strength(w) = 2.0f;
  ecs_Player_Position_x(w)[0] = 0;
  ecs_Player_Position_y(w)[0] = 0;
  ecs_Player_Velocity_dx(w)[0] = 1;
  ecs_Player_Velocity_dy(w)[0] = 1;
  const float dt = 1.0f / 60.0f;
#define FRAME() ecs_frame(w, dt)

  for (int f = 0; f < VALIDATION_FRAMES; ++f)
    FRAME();
  double checksum = 0;
  for (int k = 0; k < NUM_COLUMNS; ++k)
    for (int64_t i = 0; i < n; ++i)
      checksum += c[k][i];
  checksum += (double)ecs_Player_Position_x(w)[0] +
              (double)ecs_Player_Position_y(w)[0] +
              (double)*ecs_Clock_frame(w);

  // Calibrate (this also warms caches and the OpenMP thread pool): double
  // the frame count until a batch takes at least a tenth of the target.
  int64_t frames = 1;
  for (;;) {
    uint64_t start = now();
    for (int64_t f = 0; f < frames; ++f)
      FRAME();
    uint64_t elapsed = now() - start;
    if (elapsed * 10 >= target) {
      frames = (int64_t)((double)frames * target / elapsed) + 1;
      break;
    }
    frames *= 2;
  }

  uint64_t best = UINT64_MAX;
  for (int r = 0; r < reps; ++r) {
    uint64_t start = now();
    for (int64_t f = 0; f < frames; ++f)
      FRAME();
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }

  printf("n=%lld frames=%lld ns_per_frame=%.1f checksum=%.17g\n",
         (long long)n, (long long)frames, (double)best / frames, checksum);
  return 0;
}
