// Host program for examples/integrate.mlir. The world's layout lives in
// the generated header (ecs-translate --ecs-to-c-header), so the host only
// creates the world, fills the entities it wants, runs a few frames and
// prints every position.

#include "integrate_world.h"

#include <stdio.h>

enum { BODIES = 2, PARTICLES = 2, SCENERY = 1, PLAYERS = 1, FRAMES = 3 };

static void fill(float *column, const float *values, int n) {
  for (int i = 0; i < n; ++i)
    column[i] = values[i];
}

int main(void) {
  ecs_world *w = ecs_world_create();
  if (!w || !ecs_Body_set_count(w, BODIES) ||
      !ecs_Particle_set_count(w, PARTICLES) ||
      !ecs_Scenery_set_count(w, SCENERY) ||
      !ecs_Player_set_count(w, PLAYERS)) {
    fprintf(stderr, "could not set up the world\n");
    return 1;
  }

  fill(ecs_Body_Position_x(w), (float[]){0, 10}, BODIES);
  fill(ecs_Body_Position_y(w), (float[]){100, 50}, BODIES);
  fill(ecs_Body_Velocity_dx(w), (float[]){1, -2}, BODIES);
  fill(ecs_Body_Velocity_dy(w), (float[]){0, 4}, BODIES);
  fill(ecs_Body_Mass_kg(w), (float[]){1, 5}, BODIES);
  fill(ecs_Particle_Position_x(w), (float[]){0, 3}, PARTICLES);
  fill(ecs_Particle_Position_y(w), (float[]){0, 3}, PARTICLES);
  fill(ecs_Particle_Velocity_dx(w), (float[]){2, 0}, PARTICLES);
  fill(ecs_Particle_Velocity_dy(w), (float[]){1, -1}, PARTICLES);
  fill(ecs_Particle_Lifetime_seconds(w), (float[]){10, 2}, PARTICLES);
  fill(ecs_Scenery_Position_x(w), (float[]){7}, SCENERY);
  fill(ecs_Scenery_Position_y(w), (float[]){7}, SCENERY);
  fill(ecs_Player_Position_x(w), (float[]){0}, PLAYERS);
  fill(ecs_Player_Position_y(w), (float[]){0}, PLAYERS);
  fill(ecs_Player_Velocity_dx(w), (float[]){1}, PLAYERS);
  fill(ecs_Player_Velocity_dy(w), (float[]){1}, PLAYERS);
  *ecs_Wind_strength(w) = 2.0f;

  for (int frame = 0; frame < FRAMES; ++frame)
    ecs_frame(w, 0.5f);

  for (int i = 0; i < BODIES; ++i)
    printf("body %d: pos (%.4f, %.4f) vel (%.4f, %.4f)\n", i,
           ecs_Body_Position_x(w)[i], ecs_Body_Position_y(w)[i],
           ecs_Body_Velocity_dx(w)[i], ecs_Body_Velocity_dy(w)[i]);
  for (int i = 0; i < PARTICLES; ++i)
    printf("particle %d: pos (%.4f, %.4f) vel (%.4f, %.4f) life %.4f\n", i,
           ecs_Particle_Position_x(w)[i], ecs_Particle_Position_y(w)[i],
           ecs_Particle_Velocity_dx(w)[i], ecs_Particle_Velocity_dy(w)[i],
           ecs_Particle_Lifetime_seconds(w)[i]);
  for (int i = 0; i < SCENERY; ++i)
    printf("scenery %d: pos (%.4f, %.4f)\n", i, ecs_Scenery_Position_x(w)[i],
           ecs_Scenery_Position_y(w)[i]);
  printf("player: pos (%.4f, %.4f)\n", ecs_Player_Position_x(w)[0],
         ecs_Player_Position_y(w)[0]);
  printf("frame: %lld\n", (long long)*ecs_Clock_frame(w));

  ecs_world_destroy(w);
  return 0;
}
