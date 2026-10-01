// Host for spawn_unvisited.mlir: one @A entity, one frame.

#include "spawn_unvisited_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  int64_t row = ent_entity_row(w, ent_A_spawn(w));
  ent_A_T_visits(w)[row] = 0;
  ent_frame(w);
  printf("A: %lld entities, visits %d; B: %lld entities, visits",
         (long long)ent_A_count(w), ent_A_T_visits(w)[0],
         (long long)ent_B_count(w));
  for (int64_t i = 0; i < ent_B_count(w); ++i)
    printf(" %d", ent_B_T_visits(w)[i]);
  printf("\n");
  ent_world_destroy(w);
  return 0;
}
