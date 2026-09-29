// Host program for examples/status.mlir: three characters, five frames of
// dt = 0.5, then every character's position, velocity and stun.

#include "status_world.h"

#include <stdio.h>

enum { CHARACTERS = 3, FRAMES = 5 };

int main(void) {
  ecs_world *w = ecs_world_create();
  // Characters start without the optional Stunned component.
  if (!w || !ecs_Character_set_count(w, CHARACTERS)) {
    fprintf(stderr, "could not set up the world\n");
    return 1;
  }
  const float x[CHARACTERS] = {0, 9, 5}, dx[CHARACTERS] = {4, 4, -1};
  for (int i = 0; i < CHARACTERS; ++i) {
    ecs_Character_Position_x(w)[i] = x[i];
    ecs_Character_Velocity_dx(w)[i] = dx[i];
  }

  for (int frame = 0; frame < FRAMES; ++frame)
    ecs_frame(w, 0.5f);

  for (int i = 0; i < CHARACTERS; ++i) {
    printf("character %d: x %.4f dx %.4f", i, ecs_Character_Position_x(w)[i],
           ecs_Character_Velocity_dx(w)[i]);
    if (ecs_Character_Stunned_present(w)[i])
      printf(" stunned %.4f s\n", ecs_Character_Stunned_seconds(w)[i]);
    else
      printf(" not stunned\n");
  }
  ecs_world_destroy(w);
  return 0;
}
