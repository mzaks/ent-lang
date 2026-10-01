// Stable ids across a despawn: the despawned id stops being alive, the
// entity moved into its row keeps its id with a new row, and the freed
// slot is reused with the next generation.
#include "ids_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity a = ent_A_spawn(w), b = ent_A_spawn(w), c = ent_A_spawn(w);
  ent_A_P_flag(w)[ent_entity_row(w, a)] = 1;
  ent_A_P_flag(w)[ent_entity_row(w, b)] = 0;
  ent_A_P_flag(w)[ent_entity_row(w, c)] = 0;
  printf("before: a %llx b %llx c %llx\n", (unsigned long long)a,
         (unsigned long long)b, (unsigned long long)c);

  ent_frame(w);

  printf("count %lld\n", (long long)ent_A_count(w));
  printf("a alive %d\n", ent_entity_alive(w, a));
  printf("b alive %d row %lld\n", ent_entity_alive(w, b),
         (long long)ent_entity_row(w, b));
  printf("c alive %d row %lld\n", ent_entity_alive(w, c),
         (long long)ent_entity_row(w, c));
  ent_entity d = ent_A_spawn(w);
  printf("d %llx row %lld\n", (unsigned long long)d,
         (long long)ent_entity_row(w, d));
  printf("a alive %d\n", ent_entity_alive(w, a));
  ent_world_destroy(w);
  return 0;
}
