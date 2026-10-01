// Host for relations.mlir: prints each neuron's pushed and pulled input.

#include "relations_world.h"
#include <stdio.h>
int main(void) {
  ent_world *w = ent_world_create();
  for (int f = 0; f < 2; ++f) {
    ent_frame(w);
    for (int i = 0; i < 3; ++i)
      printf("frame %d n%d: input %.2f gathered %.2f\n", f, i,
             ent_N_archetype_N_input(w)[i], ent_N_archetype_N_gathered(w)[i]);
  }
  ent_world_destroy(w);
  return 0;
}
