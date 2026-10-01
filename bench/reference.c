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

void _mlir_ciface_frame(float dt, ent_arena_descriptor *arena) {
  ent_world *w = (ent_world *)arena->aligned;
  bodies(dt, 9.81f, ent_Body_count(w), ent_Body_Position_x(w),
         ent_Body_Position_y(w), ent_Body_Velocity_dx(w),
         ent_Body_Velocity_dy(w));
  particles(dt, *ent_Wind_strength(w), ent_Particle_count(w),
            ent_Particle_Position_x(w), ent_Particle_Position_y(w),
            ent_Particle_Velocity_dx(w), ent_Particle_Velocity_dy(w),
            ent_Particle_Lifetime_seconds(w));
  if (ent_Player_count(w) > 0) {
    ent_Player_Position_x(w)[0] += ent_Player_Velocity_dx(w)[0] * dt;
    ent_Player_Position_y(w)[0] += ent_Player_Velocity_dy(w)[0] * dt;
  }
  *ent_Clock_frame(w) += 1;
}
