// Host for reactive_moves.mlir: the same ships and frames as
// examples/host/reactive_main.c; ships without a shield live in @Ship,
// shielded ones in @Shielded.

#include "reactive_moves_world.h"

#include <stdio.h>

static ecs_entity spawnShip(ecs_world *w) {
  ecs_entity id = ecs_Ship_spawn(w);
  int64_t row = ecs_entity_row(w, id);
  ecs_Ship_Hull_hp(w)[row] = 99;
  ecs_Ship_Hit_damage(w)[row] = 0;
  ecs_Ship_Bar_width(w)[row] = 0;
  ecs_Ship_Bar_redraws(w)[row] = 0;
  ecs_Ship_Bar_alarms(w)[row] = 0;
  ecs_Ship_Bar_restores(w)[row] = 0;
  return id;
}

// Accessors for whichever archetype the ship is in.
#define FIELD(w, id, component, field)                                        \
  (ecs_entity_archetype(w, id) == ECS_ARCHETYPE_Shielded                      \
       ? ecs_Shielded_##component##_##field(w)                               \
       : ecs_Ship_##component##_##field(w))[ecs_entity_row(w, id)]

static void report(ecs_world *w, int frame, ecs_entity *ships) {
  for (int i = 0; i < 3; ++i) {
    ecs_entity id = ships[i];
    printf("frame %d ship %d: hp %.0f width %.0f redraws %d alarms %d "
           "restores %d shield %d\n",
           frame, i, FIELD(w, id, Hull, hp), FIELD(w, id, Bar, width),
           FIELD(w, id, Bar, redraws), FIELD(w, id, Bar, alarms),
           FIELD(w, id, Bar, restores),
           ecs_entity_archetype(w, id) == ECS_ARCHETYPE_Shielded);
  }
}

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity ships[3] = {spawnShip(w), spawnShip(w), spawnShip(w)};
  for (int frame = 0; frame < 4; ++frame) {
    if (frame == 0)
      for (int i = 0; i < 3; ++i)
        FIELD(w, ships[i], Hit, damage) = -1;
    if (frame == 1)
      FIELD(w, ships[1], Hit, damage) = 60;
    if (frame == 2)
      FIELD(w, ships[1], Hit, damage) = -60;
    ecs_frame(w);
    report(w, frame, ships);
  }
  ecs_world_destroy(w);
  return 0;
}
