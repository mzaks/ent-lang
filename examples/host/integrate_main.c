// Host program for examples/integrate.mlir. The world's layout lives in
// the generated header (ent-translate --ent-to-c-header), so the host only
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
  ent_world *w = ent_world_create();
  if (!w || !ent_Body_spawn_n(w, BODIES) ||
      !ent_Particle_spawn_n(w, PARTICLES) ||
      !ent_Scenery_spawn_n(w, SCENERY) ||
      !ent_Player_spawn_n(w, PLAYERS)) {
    fprintf(stderr, "could not set up the world\n");
    return 1;
  }

  fill(ent_Body_Position_x(w), (float[]){0, 10}, BODIES);
  fill(ent_Body_Position_y(w), (float[]){100, 50}, BODIES);
  fill(ent_Body_Velocity_dx(w), (float[]){1, -2}, BODIES);
  fill(ent_Body_Velocity_dy(w), (float[]){0, 4}, BODIES);
  fill(ent_Body_Mass_kg(w), (float[]){1, 5}, BODIES);
  fill(ent_Particle_Position_x(w), (float[]){0, 3}, PARTICLES);
  fill(ent_Particle_Position_y(w), (float[]){0, 3}, PARTICLES);
  fill(ent_Particle_Velocity_dx(w), (float[]){2, 0}, PARTICLES);
  fill(ent_Particle_Velocity_dy(w), (float[]){1, -1}, PARTICLES);
  fill(ent_Particle_Lifetime_seconds(w), (float[]){10, 2}, PARTICLES);
  fill(ent_Scenery_Position_x(w), (float[]){7}, SCENERY);
  fill(ent_Scenery_Position_y(w), (float[]){7}, SCENERY);
  fill(ent_Player_Position_x(w), (float[]){0}, PLAYERS);
  fill(ent_Player_Position_y(w), (float[]){0}, PLAYERS);
  fill(ent_Player_Velocity_dx(w), (float[]){1}, PLAYERS);
  fill(ent_Player_Velocity_dy(w), (float[]){1}, PLAYERS);
  *ent_Wind_strength(w) = 2.0f;

  for (int frame = 0; frame < FRAMES; ++frame)
    ent_frame(w, 0.5f);

  for (int i = 0; i < BODIES; ++i)
    printf("body %d: pos (%.4f, %.4f) vel (%.4f, %.4f)\n", i,
           ent_Body_Position_x(w)[i], ent_Body_Position_y(w)[i],
           ent_Body_Velocity_dx(w)[i], ent_Body_Velocity_dy(w)[i]);
  for (int i = 0; i < PARTICLES; ++i)
    printf("particle %d: pos (%.4f, %.4f) vel (%.4f, %.4f) life %.4f\n", i,
           ent_Particle_Position_x(w)[i], ent_Particle_Position_y(w)[i],
           ent_Particle_Velocity_dx(w)[i], ent_Particle_Velocity_dy(w)[i],
           ent_Particle_Lifetime_seconds(w)[i]);
  for (int i = 0; i < SCENERY; ++i)
    printf("scenery %d: pos (%.4f, %.4f)\n", i, ent_Scenery_Position_x(w)[i],
           ent_Scenery_Position_y(w)[i]);
  printf("player: pos (%.4f, %.4f)\n", ent_Player_Position_x(w)[0],
         ent_Player_Position_y(w)[0]);
  printf("frame: %lld\n", (long long)*ent_Clock_frame(w));

  ent_world_destroy(w);
  return 0;
}
