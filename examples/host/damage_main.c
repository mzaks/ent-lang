// Host program for examples/damage.mlir: two ships, five torpedoes, two
// frames of dt = 0.5. After frame 0 a new ship is spawned; it takes the
// slot of the ship that sank (the last one freed), and must not be hit by
// the torpedo still aimed at that ship.

#include "damage_world.h"

#include <stdio.h>

static ecs_entity spawnShip(ecs_world *w, float hp) {
  ecs_entity id = ecs_Ship_spawn(w);
  ecs_Ship_Hull_hp(w)[ecs_entity_row(w, id)] = hp;
  return id;
}

static void spawnTorpedo(ecs_world *w, ecs_entity target, float damage,
                         float seconds) {
  ecs_entity id = ecs_Torpedo_spawn(w);
  int64_t row = ecs_entity_row(w, id);
  ecs_Torpedo_Fuse_seconds(w)[row] = seconds;
  ecs_Torpedo_Warhead_damage(w)[row] = damage;
  ecs_Torpedo_Target_entity(w)[row] = target;
}

static void report(ecs_world *w, const char *name, ecs_entity ship) {
  if (!ecs_entity_alive(w, ship)) {
    printf("%s: gone\n", name);
    return;
  }
  printf("%s: hp %.4f\n", name,
         ecs_Ship_Hull_hp(w)[ecs_entity_row(w, ship)]);
}

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity ships[3] = {spawnShip(w, 100), spawnShip(w, 30), 0};
  spawnTorpedo(w, ships[0], 40, 0.5f);
  spawnTorpedo(w, ships[0], 40, 0.5f);
  spawnTorpedo(w, ships[1], 50, 0.5f);
  spawnTorpedo(w, ships[1], 50, 1.0f);
  spawnTorpedo(w, ships[0], 10, 0.5f);

  for (int frame = 0; frame < 2; ++frame) {
    ecs_frame(w, 0.5f);
    printf("frame %d: ships %lld torpedoes %lld\n", frame,
           (long long)ecs_Ship_count(w), (long long)ecs_Torpedo_count(w));
    if (frame == 0) {
      ships[2] = spawnShip(w, 100);
      printf("new ship reuses a slot: %s\n",
             ecs__slot(ships[2]) == ecs__slot(ships[1]) ? "yes" : "no");
    }
  }

  report(w, "ship 0", ships[0]);
  report(w, "ship 1", ships[1]);
  report(w, "ship 2", ships[2]);
  ecs_world_destroy(w);
  return 0;
}
