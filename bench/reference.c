// Hand-written reference for examples/integrate.mlir: the fused frame as a
// C programmer would write it, on the same world (generated header) and
// with the same entry point as the lowered schedule. Compile with
// -ffp-contract=off so the arithmetic matches the generated code exactly
// (no fused multiply-add). Define RESTRICT=restrict to tell the compiler
// that columns do not alias.

#include "integrate_world.h"

#ifndef RESTRICT
#define RESTRICT
#endif

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

void _mlir_ciface_frame(float dt, ecs_arena_descriptor *arena) {
  ecs_world *w = (ecs_world *)arena->aligned;
  bodies(dt, 9.81f, ecs_Body_count(w), ecs_Body_Position_x(w),
         ecs_Body_Position_y(w), ecs_Body_Velocity_dx(w),
         ecs_Body_Velocity_dy(w));
  particles(dt, 2.0f, ecs_Particle_count(w), ecs_Particle_Position_x(w),
            ecs_Particle_Position_y(w), ecs_Particle_Velocity_dx(w),
            ecs_Particle_Velocity_dy(w), ecs_Particle_Lifetime_seconds(w));
}
