// Host program for examples/status_moves.mlir: the same three characters
// as status_main.c. Characters move between archetypes while the frames
// run, so the host keeps their ids and looks each one up afterwards.

#include "status_moves_world.h"

#include <stdio.h>

enum { CHARACTERS = 3, FRAMES = 5 };

int main(void) {
  ent_world *w = ent_world_create();
  if (!w)
    return 1;
  const float x[CHARACTERS] = {0, 9, 5}, dx[CHARACTERS] = {4, 4, -1};
  ent_entity ids[CHARACTERS];
  for (int i = 0; i < CHARACTERS; ++i) {
    ids[i] = ent_Character_spawn(w);
    int64_t row = ent_entity_row(w, ids[i]);
    ent_Character_Position_x(w)[row] = x[i];
    ent_Character_Velocity_dx(w)[row] = dx[i];
  }

  for (int frame = 0; frame < FRAMES; ++frame)
    ent_frame(w, 0.5f);

  for (int i = 0; i < CHARACTERS; ++i) {
    int64_t row = ent_entity_row(w, ids[i]);
    if (ent_entity_archetype(w, ids[i]) == ENT_ARCHETYPE_StunnedCharacter)
      printf("character %d: x %.4f dx %.4f stunned %.4f s\n", i,
             ent_StunnedCharacter_Position_x(w)[row],
             ent_StunnedCharacter_Velocity_dx(w)[row],
             ent_StunnedCharacter_Stunned_seconds(w)[row]);
    else
      printf("character %d: x %.4f dx %.4f not stunned\n", i,
             ent_Character_Position_x(w)[row],
             ent_Character_Velocity_dx(w)[row]);
  }
  printf("characters: %lld not stunned, %lld stunned\n",
         (long long)ent_Character_count(w),
         (long long)ent_StunnedCharacter_count(w));
  ent_world_destroy(w);
  return 0;
}
