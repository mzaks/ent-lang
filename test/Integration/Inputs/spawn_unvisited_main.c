// Host for spawn_unvisited.mlir: one @A entity, one frame.

#include "spawn_unvisited_world.h"

#include <stdio.h>

int main(void) {
  ecs_world *w = ecs_world_create();
  int64_t row = ecs_entity_row(w, ecs_A_spawn(w));
  ecs_A_T_visits(w)[row] = 0;
  ecs_frame(w);
  printf("A: %lld entities, visits %d; B: %lld entities, visits",
         (long long)ecs_A_count(w), ecs_A_T_visits(w)[0],
         (long long)ecs_B_count(w));
  for (int64_t i = 0; i < ecs_B_count(w); ++i)
    printf(" %d", ecs_B_T_visits(w)[i]);
  printf("\n");
  ecs_world_destroy(w);
  return 0;
}
