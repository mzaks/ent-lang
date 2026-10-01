// Host for reactive_moves.mlir: the same ships and frames as
// examples/host/reactive_main.c; ships without a shield live in @Ship,
// shielded ones in @Shielded.

#include "reactive_moves_world.h"

#include <stdio.h>

static ent_entity spawnShip(ent_world *w) {
  ent_entity id = ent_Ship_spawn(w);
  int64_t row = ent_entity_row(w, id);
  ent_Ship_Hull_hp(w)[row] = 99;
  ent_Ship_Hit_damage(w)[row] = 0;
  ent_Ship_Bar_width(w)[row] = 0;
  ent_Ship_Bar_redraws(w)[row] = 0;
  ent_Ship_Bar_alarms(w)[row] = 0;
  ent_Ship_Bar_restores(w)[row] = 0;
  return id;
}

// Accessors for whichever archetype the ship is in.
#define FIELD(w, id, component, field)                                        \
  (ent_entity_archetype(w, id) == ENT_ARCHETYPE_Shielded                      \
       ? ent_Shielded_##component##_##field(w)                               \
       : ent_Ship_##component##_##field(w))[ent_entity_row(w, id)]

static void report(ent_world *w, int frame, ent_entity *ships) {
  for (int i = 0; i < 3; ++i) {
    ent_entity id = ships[i];
    printf("frame %d ship %d: hp %.0f width %.0f redraws %d alarms %d "
           "restores %d shield %d\n",
           frame, i, FIELD(w, id, Hull, hp), FIELD(w, id, Bar, width),
           FIELD(w, id, Bar, redraws), FIELD(w, id, Bar, alarms),
           FIELD(w, id, Bar, restores),
           ent_entity_archetype(w, id) == ENT_ARCHETYPE_Shielded);
  }
}

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity ships[3] = {spawnShip(w), spawnShip(w), spawnShip(w)};
  for (int frame = 0; frame < 4; ++frame) {
    if (frame == 0)
      for (int i = 0; i < 3; ++i)
        FIELD(w, ships[i], Hit, damage) = -1;
    if (frame == 1)
      FIELD(w, ships[1], Hit, damage) = 60;
    if (frame == 2)
      FIELD(w, ships[1], Hit, damage) = -60;
    ent_frame(w);
    report(w, frame, ships);
  }
  ent_world_destroy(w);
  return 0;
}
