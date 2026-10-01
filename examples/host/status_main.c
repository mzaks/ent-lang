// Host program for examples/status.mlir: three characters, five frames of
// dt = 0.5, then every character's position, velocity and stun.

#include "status_world.h"

#include <stdio.h>

enum { CHARACTERS = 3, FRAMES = 5 };

int main(void) {
  ent_world *w = ent_world_create();
  // Spawned characters start without the optional Stunned component.
  if (!w || !ent_Character_spawn_n(w, CHARACTERS)) {
    fprintf(stderr, "could not set up the world\n");
    return 1;
  }
  const float x[CHARACTERS] = {0, 9, 5}, dx[CHARACTERS] = {4, 4, -1};
  for (int i = 0; i < CHARACTERS; ++i) {
    ent_Character_Position_x(w)[i] = x[i];
    ent_Character_Velocity_dx(w)[i] = dx[i];
  }

  for (int frame = 0; frame < FRAMES; ++frame)
    ent_frame(w, 0.5f);

  for (int i = 0; i < CHARACTERS; ++i) {
    printf("character %d: x %.4f dx %.4f", i, ent_Character_Position_x(w)[i],
           ent_Character_Velocity_dx(w)[i]);
    if (ent_Character_Stunned_present(w)[i])
      printf(" stunned %.4f s\n", ent_Character_Stunned_seconds(w)[i]);
    else
      printf(" not stunned\n");
  }
  ent_world_destroy(w);
  return 0;
}
