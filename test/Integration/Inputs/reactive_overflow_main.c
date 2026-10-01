// Host for reactive_overflow.mlir: four units; frame 1 hurts all of them,
// frames 0 and 2 none.

#include "reactive_overflow_world.h"

#include <stdio.h>

int main(void) {
  ecs_world *w = ecs_world_create();
  for (int i = 0; i < 4; ++i) {
    int64_t row = ecs_entity_row(w, ecs_Unit_spawn(w));
    ecs_Unit_Hull_hp(w)[row] = 10;
    ecs_Unit_Seen_count(w)[row] = 0;
  }
  for (int frame = 0; frame < 3; ++frame) {
    ecs_frame(w, frame == 1);
    printf("frame %d:", frame);
    for (int64_t row = 0; row < ecs_Unit_count(w); ++row)
      printf(" %d", ecs_Unit_Seen_count(w)[row]);
    printf("\n");
  }
  ecs_world_destroy(w);
  return 0;
}
