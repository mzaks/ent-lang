// Spawns one entity per frame into an archetype of capacity 2.
#include "capacity_world.h"

#include <stdio.h>

int main(void) {
  ecs_world *w = ecs_world_create();
  for (int i = 0; i < 3; ++i) {
    ecs_frame(w, (float)i);
    printf("count %lld\n", (long long)ecs_A_count(w));
    fflush(stdout);
  }
  return 0;
}
