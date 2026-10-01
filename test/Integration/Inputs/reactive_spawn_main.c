// Host for reactive_spawn.mlir: two units before the first frame (its
// first run sees them all), one more from the host before frame 1, one
// from the program in frame 1, nothing in frame 2.

#include "reactive_spawn_world.h"

#include <stdio.h>

static void spawn(ent_world *w) {
  int64_t row = ent_entity_row(w, ent_Unit_spawn(w));
  ent_Unit_Hull_hp(w)[row] = 10;
  ent_Unit_Seen_count(w)[row] = 0;
}

int main(void) {
  ent_world *w = ent_world_create();
  spawn(w);
  spawn(w);
  for (int frame = 0; frame < 3; ++frame) {
    if (frame == 1)
      spawn(w);
    ent_frame(w, frame == 1);
    printf("frame %d:", frame);
    for (int64_t row = 0; row < ent_Unit_count(w); ++row)
      printf(" %d", ent_Unit_Seen_count(w)[row]);
    printf("\n");
  }
  ent_world_destroy(w);
  return 0;
}
