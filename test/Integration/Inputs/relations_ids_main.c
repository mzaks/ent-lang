// Host for relations_ids.mlir.

#include "relations_ids_world.h"
#include <stdio.h>

static ent_entity cells[5];
static int alive[5];

static void spawn(ent_world *w, int i, float v) {
  cells[i] = ent_Cell_spawn(w);
  int64_t row = ent_entity_row(w, cells[i]);
  ent_Cell_N_v(w)[row] = v;
  ent_Cell_N_input(w)[row] = 0;
  alive[i] = 1;
}

int main(void) {
  ent_world *w = ent_world_create();
  for (int i = 0; i < 4; ++i)
    spawn(w, i, (float)(i + 1));
  for (int i = 0; i < 4; ++i)
    ent_Cell_N_next(w)[ent_entity_row(w, cells[i])] = cells[(i + 1) % 4];
  ent_Syn_connect(w, cells[0], cells[2], 10.0f);
  ent_Syn_connect(w, cells[3], cells[1], 0.5f);
  for (int f = 0; f < 3; ++f) {
    // Frame 2: a new cell takes the freed slot of cell 2.
    if (f == 2)
      spawn(w, 4, 5.0f);
    ent_frame(w);
    printf("frame %d: edges %lld", f, (long long)ent_Syn_count(w));
    for (int i = 0; i < 5; ++i) {
      if (!ent_entity_alive(w, cells[i]) || !alive[i]) {
        continue;
      }
      printf(" c%d %.0f", i, ent_Cell_N_input(w)[ent_entity_row(w, cells[i])]);
    }
    printf("\n");
  }
  printf("slot reused: %d\n",
         (int)((cells[4] & ((1u << ENT__SLOT_BITS) - 1)) ==
               (cells[2] & ((1u << ENT__SLOT_BITS) - 1))));
  ent_world_destroy(w);
  return 0;
}
