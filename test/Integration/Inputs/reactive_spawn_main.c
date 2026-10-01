// Host for reactive_spawn.mlir: two units before the first frame (its
// first run sees them all), one more from the host before frame 1, one
// from the program in frame 1, nothing in frame 2.

#include "reactive_spawn_world.h"

#include <stdio.h>

static void spawn(ecs_world *w) {
  int64_t row = ecs_entity_row(w, ecs_Unit_spawn(w));
  ecs_Unit_Hull_hp(w)[row] = 10;
  ecs_Unit_Seen_count(w)[row] = 0;
}

int main(void) {
  ecs_world *w = ecs_world_create();
  spawn(w);
  spawn(w);
  for (int frame = 0; frame < 3; ++frame) {
    if (frame == 1)
      spawn(w);
    ecs_frame(w, frame == 1);
    printf("frame %d:", frame);
    for (int64_t row = 0; row < ecs_Unit_count(w); ++row)
      printf(" %d", ecs_Unit_Seen_count(w)[row]);
    printf("\n");
  }
  ecs_world_destroy(w);
  return 0;
}
