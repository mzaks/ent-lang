// Host program for examples/status_moves.mlir: the same three characters
// as status_main.c. Characters move between archetypes while the frames
// run, so the host keeps their ids and looks each one up afterwards.

#include "status_moves_world.h"

#include <stdio.h>

enum { CHARACTERS = 3, FRAMES = 5 };

int main(void) {
  ecs_world *w = ecs_world_create();
  if (!w)
    return 1;
  const float x[CHARACTERS] = {0, 9, 5}, dx[CHARACTERS] = {4, 4, -1};
  ecs_entity ids[CHARACTERS];
  for (int i = 0; i < CHARACTERS; ++i) {
    ids[i] = ecs_Character_spawn(w);
    int64_t row = ecs_entity_row(w, ids[i]);
    ecs_Character_Position_x(w)[row] = x[i];
    ecs_Character_Velocity_dx(w)[row] = dx[i];
  }

  for (int frame = 0; frame < FRAMES; ++frame)
    ecs_frame(w, 0.5f);

  for (int i = 0; i < CHARACTERS; ++i) {
    int64_t row = ecs_entity_row(w, ids[i]);
    if (ecs_entity_archetype(w, ids[i]) == ECS_ARCHETYPE_StunnedCharacter)
      printf("character %d: x %.4f dx %.4f stunned %.4f s\n", i,
             ecs_StunnedCharacter_Position_x(w)[row],
             ecs_StunnedCharacter_Velocity_dx(w)[row],
             ecs_StunnedCharacter_Stunned_seconds(w)[row]);
    else
      printf("character %d: x %.4f dx %.4f not stunned\n", i,
             ecs_Character_Position_x(w)[row],
             ecs_Character_Velocity_dx(w)[row]);
  }
  printf("characters: %lld not stunned, %lld stunned\n",
         (long long)ecs_Character_count(w),
         (long long)ecs_StunnedCharacter_count(w));
  ecs_world_destroy(w);
  return 0;
}
