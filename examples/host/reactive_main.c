// Host program for examples/reactive.mlir: three ships, four frames. They
// start without shields at 99 hp; frame 0 repairs each by 1, which gives
// it a shield (the program adds it, so reactive queries see it; a host
// writing the presence byte would not be tracked). Frame 1 damages ship 1
// below 50 hp, so its shield fails; frame 2 repairs it, which restores the
// shield. Frame 3 changes nothing.

#include "reactive_world.h"

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

static void report(ent_world *w, int frame, ent_entity *ships) {
  for (int i = 0; i < 3; ++i) {
    int64_t row = ent_entity_row(w, ships[i]);
    printf("frame %d ship %d: hp %.0f width %.0f redraws %d alarms %d "
           "restores %d shield %d\n",
           frame, i, ent_Ship_Hull_hp(w)[row], ent_Ship_Bar_width(w)[row],
           ent_Ship_Bar_redraws(w)[row], ent_Ship_Bar_alarms(w)[row],
           ent_Ship_Bar_restores(w)[row], ent_Ship_Shield_present(w)[row]);
  }
}

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity ships[3] = {spawnShip(w), spawnShip(w), spawnShip(w)};
  for (int frame = 0; frame < 4; ++frame) {
    int64_t row = ent_entity_row(w, ships[1]);
    if (frame == 0)
      for (int i = 0; i < 3; ++i)
        ent_Ship_Hit_damage(w)[ent_entity_row(w, ships[i])] = -1;
    if (frame == 1)
      ent_Ship_Hit_damage(w)[row] = 60;
    if (frame == 2)
      ent_Ship_Hit_damage(w)[row] = -60;
    ent_frame(w);
    report(w, frame, ships);
  }
  ent_world_destroy(w);
  return 0;
}
