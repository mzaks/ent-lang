// Host for accumulate.mlir: four sources, one of them not armed.

#include "accumulate_world.h"

#include <stdio.h>

static void source(ent_world *w, float v, int n, int armed) {
  int64_t row = ent_entity_row(w, ent_Source_spawn(w));
  ent_Source_Amount_v(w)[row] = v;
  ent_Source_Amount_n(w)[row] = n;
  ent_Source_Amount_seen(w)[row] = -1;
  ent_Source_Armed_present(w)[row] = armed;
}

int main(void) {
  ent_world *w = ent_world_create();
  *ent_Total_sum(w) = 0;
  *ent_Total_lo(w) = 100;
  *ent_Total_count(w) = 0;
  source(w, 1e8f, 5, 1);
  source(w, 1, -3, 1);
  source(w, -1e8f, 7, 1);
  source(w, 1000, -50, 0); // not armed: sends nothing
  ent_frame(w);
  printf("total: sum %.1f lo %d count %lld\n", *ent_Total_sum(w),
         *ent_Total_lo(w), (long long)*ent_Total_count(w));
  printf("seen:");
  for (int i = 0; i < 4; ++i)
    printf(" %.1f", ent_Source_Amount_seen(w)[i]);
  printf("\n");
  ent_world_destroy(w);
  return 0;
}
