// Host for relations_typed.mlir: only entity 2 has a @Hub, so edges to the
// others are refused.

#include "relations_typed_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity e[3];
  for (int i = 0; i < 3; ++i) {
    e[i] = ent_A_spawn(w);
    ent_A_N_v(w)[i] = 0.0f;
    ent_A_Hub_present(w)[i] = i == 2;
  }
  ent_A_Hub_h(w)[2] = 5.0f;
  printf("0 -> 2: %d\n", ent_Feeds_connect(w, e[0], e[2], 1.0f));
  printf("0 -> 1: %d\n", ent_Feeds_connect(w, e[0], e[1], 1.0f));
  printf("1 -> 2: %d\n", ent_Feeds_connect(w, e[1], e[2], 2.0f));
  ent_frame(w);
  printf("edges %lld v %.1f %.1f %.1f\n", (long long)ent_Feeds_count(w),
         ent_A_N_v(w)[0], ent_A_N_v(w)[1], ent_A_N_v(w)[2]);
  ent_world_destroy(w);
  return 0;
}
