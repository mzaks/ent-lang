// Stable ids across a despawn: the despawned id stops being alive, the
// entity moved into its row keeps its id with a new row, and the freed
// slot is reused with the next generation.
#include "ids_world.h"

#include <stdio.h>

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity a = ecs_A_spawn(w), b = ecs_A_spawn(w), c = ecs_A_spawn(w);
  ecs_A_P_flag(w)[ecs_entity_row(w, a)] = 1;
  ecs_A_P_flag(w)[ecs_entity_row(w, b)] = 0;
  ecs_A_P_flag(w)[ecs_entity_row(w, c)] = 0;
  printf("before: a %llx b %llx c %llx\n", (unsigned long long)a,
         (unsigned long long)b, (unsigned long long)c);

  ecs_frame(w);

  printf("count %lld\n", (long long)ecs_A_count(w));
  printf("a alive %d\n", ecs_entity_alive(w, a));
  printf("b alive %d row %lld\n", ecs_entity_alive(w, b),
         (long long)ecs_entity_row(w, b));
  printf("c alive %d row %lld\n", ecs_entity_alive(w, c),
         (long long)ecs_entity_row(w, c));
  ecs_entity d = ecs_A_spawn(w);
  printf("d %llx row %lld\n", (unsigned long long)d,
         (long long)ecs_entity_row(w, d));
  printf("a alive %d\n", ecs_entity_alive(w, a));
  ecs_world_destroy(w);
  return 0;
}
