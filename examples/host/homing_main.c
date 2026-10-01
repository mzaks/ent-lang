// Host program for examples/homing.mlir: two ships, two missiles aimed at
// one ship each, four frames of dt = 0.5.

#include "homing_world.h"

#include <stdio.h>

static ent_entity spawnShip(ent_world *w, float x, float dx) {
  ent_entity id = ent_Ship_spawn(w);
  int64_t row = ent_entity_row(w, id);
  ent_Ship_Position_x(w)[row] = x;
  ent_Ship_Velocity_dx(w)[row] = dx;
  ent_Ship_Hull_hp(w)[row] = 100;
  return id;
}

static ent_entity spawnMissile(ent_world *w, ent_entity target) {
  ent_entity id = ent_Missile_spawn(w);
  int64_t row = ent_entity_row(w, id);
  ent_Missile_Position_x(w)[row] = 0;
  ent_Missile_Velocity_dx(w)[row] = 0;
  ent_Missile_Target_entity(w)[row] = target;
  return id;
}

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity ships[2] = {spawnShip(w, 10, 1), spawnShip(w, 15, 4)};
  ent_entity missiles[2] = {spawnMissile(w, ships[0]),
                            spawnMissile(w, ships[1])};

  for (int frame = 0; frame < 4; ++frame)
    ent_frame(w, 0.5f);

  printf("ships: %lld\n", (long long)ent_Ship_count(w));
  for (int i = 0; i < 2; ++i) {
    if (!ent_entity_alive(w, ships[i])) {
      printf("ship %d: gone\n", i);
      continue;
    }
    printf("ship %d: x %.4f\n", i,
           ent_Ship_Position_x(w)[ent_entity_row(w, ships[i])]);
  }
  for (int i = 0; i < 2; ++i) {
    int64_t row = ent_entity_row(w, missiles[i]);
    ent_entity target = ent_Missile_Target_entity(w)[row];
    printf("missile %d: x %.4f dx %.4f target %s\n", i,
           ent_Missile_Position_x(w)[row], ent_Missile_Velocity_dx(w)[row],
           ent_entity_alive(w, target) ? "alive" : "gone");
  }
  ent_world_destroy(w);
  return 0;
}
