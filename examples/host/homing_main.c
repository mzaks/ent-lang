// Host program for examples/homing.mlir: two ships, two missiles aimed at
// one ship each, four frames of dt = 0.5.

#include "homing_world.h"

#include <stdio.h>

static ecs_entity spawnShip(ecs_world *w, float x, float dx) {
  ecs_entity id = ecs_Ship_spawn(w);
  int64_t row = ecs_entity_row(w, id);
  ecs_Ship_Position_x(w)[row] = x;
  ecs_Ship_Velocity_dx(w)[row] = dx;
  ecs_Ship_Hull_hp(w)[row] = 100;
  return id;
}

static ecs_entity spawnMissile(ecs_world *w, ecs_entity target) {
  ecs_entity id = ecs_Missile_spawn(w);
  int64_t row = ecs_entity_row(w, id);
  ecs_Missile_Position_x(w)[row] = 0;
  ecs_Missile_Velocity_dx(w)[row] = 0;
  ecs_Missile_Target_entity(w)[row] = target;
  return id;
}

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity ships[2] = {spawnShip(w, 10, 1), spawnShip(w, 15, 4)};
  ecs_entity missiles[2] = {spawnMissile(w, ships[0]),
                            spawnMissile(w, ships[1])};

  for (int frame = 0; frame < 4; ++frame)
    ecs_frame(w, 0.5f);

  printf("ships: %lld\n", (long long)ecs_Ship_count(w));
  for (int i = 0; i < 2; ++i) {
    if (!ecs_entity_alive(w, ships[i])) {
      printf("ship %d: gone\n", i);
      continue;
    }
    printf("ship %d: x %.4f\n", i,
           ecs_Ship_Position_x(w)[ecs_entity_row(w, ships[i])]);
  }
  for (int i = 0; i < 2; ++i) {
    int64_t row = ecs_entity_row(w, missiles[i]);
    ecs_entity target = ecs_Missile_Target_entity(w)[row];
    printf("missile %d: x %.4f dx %.4f target %s\n", i,
           ecs_Missile_Position_x(w)[row], ecs_Missile_Velocity_dx(w)[row],
           ecs_entity_alive(w, target) ? "alive" : "gone");
  }
  ecs_world_destroy(w);
  return 0;
}
