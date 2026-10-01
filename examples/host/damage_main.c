// Host program for examples/damage.mlir: two ships, five torpedoes, two
// frames of dt = 0.5. After frame 0 a new ship is spawned; it takes the
// slot of the ship that sank (the last one freed), and must not be hit by
// the torpedo still aimed at that ship.

#include "damage_world.h"

#include <stdio.h>

static ent_entity spawnShip(ent_world *w, float hp) {
  ent_entity id = ent_Ship_spawn(w);
  ent_Ship_Hull_hp(w)[ent_entity_row(w, id)] = hp;
  return id;
}

static void spawnTorpedo(ent_world *w, ent_entity target, float damage,
                         float seconds) {
  ent_entity id = ent_Torpedo_spawn(w);
  int64_t row = ent_entity_row(w, id);
  ent_Torpedo_Fuse_seconds(w)[row] = seconds;
  ent_Torpedo_Warhead_damage(w)[row] = damage;
  ent_Torpedo_Target_entity(w)[row] = target;
}

static void report(ent_world *w, const char *name, ent_entity ship) {
  if (!ent_entity_alive(w, ship)) {
    printf("%s: gone\n", name);
    return;
  }
  printf("%s: hp %.4f\n", name,
         ent_Ship_Hull_hp(w)[ent_entity_row(w, ship)]);
}

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity ships[3] = {spawnShip(w, 100), spawnShip(w, 30), 0};
  spawnTorpedo(w, ships[0], 40, 0.5f);
  spawnTorpedo(w, ships[0], 40, 0.5f);
  spawnTorpedo(w, ships[1], 50, 0.5f);
  spawnTorpedo(w, ships[1], 50, 1.0f);
  spawnTorpedo(w, ships[0], 10, 0.5f);

  for (int frame = 0; frame < 2; ++frame) {
    ent_frame(w, 0.5f);
    printf("frame %d: ships %lld torpedoes %lld\n", frame,
           (long long)ent_Ship_count(w), (long long)ent_Torpedo_count(w));
    if (frame == 0) {
      ships[2] = spawnShip(w, 100);
      printf("new ship reuses a slot: %s\n",
             ent__slot(ships[2]) == ent__slot(ships[1]) ? "yes" : "no");
    }
  }

  report(w, "ship 0", ships[0]);
  report(w, "ship 1", ships[1]);
  report(w, "ship 2", ships[2]);
  ent_world_destroy(w);
  return 0;
}
