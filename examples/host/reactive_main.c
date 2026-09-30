// Host program for examples/reactive.mlir: three ships, four frames. They
// start without shields at 99 hp; frame 0 repairs each by 1, which gives
// it a shield (the program adds it, so reactive queries see it; a host
// writing the presence byte would not be tracked). Frame 1 damages ship 1
// below 50 hp, so its shield fails; frame 2 repairs it, which restores the
// shield. Frame 3 changes nothing.

#include "reactive_world.h"

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

static void report(ecs_world *w, int frame, ecs_entity *ships) {
  for (int i = 0; i < 3; ++i) {
    int64_t row = ecs_entity_row(w, ships[i]);
    printf("frame %d ship %d: hp %.0f width %.0f redraws %d alarms %d "
           "restores %d shield %d\n",
           frame, i, ecs_Ship_Hull_hp(w)[row], ecs_Ship_Bar_width(w)[row],
           ecs_Ship_Bar_redraws(w)[row], ecs_Ship_Bar_alarms(w)[row],
           ecs_Ship_Bar_restores(w)[row], ecs_Ship_Shield_present(w)[row]);
  }
}

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity ships[3] = {spawnShip(w), spawnShip(w), spawnShip(w)};
  for (int frame = 0; frame < 4; ++frame) {
    int64_t row = ecs_entity_row(w, ships[1]);
    if (frame == 0)
      for (int i = 0; i < 3; ++i)
        ecs_Ship_Hit_damage(w)[ecs_entity_row(w, ships[i])] = -1;
    if (frame == 1)
      ecs_Ship_Hit_damage(w)[row] = 60;
    if (frame == 2)
      ecs_Ship_Hit_damage(w)[row] = -60;
    ecs_frame(w);
    report(w, frame, ships);
  }
  ecs_world_destroy(w);
  return 0;
}
