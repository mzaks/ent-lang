// Host for inferred.mlir: a unit spawned in frame 0, none in frame 1.

#include "inferred_world.h"

#include <stdio.h>

int main(void) {
  ecs_world *w = ecs_world_create();
  printf("capacity %d\n", ECS_Hull_Shield_CAPACITY);
  for (int frame = 0; frame < 2; ++frame) {
    ecs_frame(w, frame == 0);
    printf("frame %d: units %lld hp %.1f shield %d\n", frame,
           (long long)ecs_Hull_Shield_count(w), ecs_Hull_Shield_Hull_hp(w)[0],
           ecs_Hull_Shield_Shield_present(w)[0]);
  }
  ecs_world_destroy(w);
  return 0;
}
