// Host for relations_order.mlir.

#include "relations_order_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  ent_frame(w);
  for (int i = 0; i < 3; ++i)
    printf("n%d: in only %d in both %d out %d\n", i,
           ent_N_archetype_N_only(w)[i], ent_N_archetype_N_both(w)[i],
           ent_N_archetype_N_out(w)[i]);
  ent_world_destroy(w);
  return 0;
}
