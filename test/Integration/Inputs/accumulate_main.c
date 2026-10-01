// Host for accumulate.mlir: four sources, one of them not armed.

#include "accumulate_world.h"

#include <stdio.h>

static void source(ecs_world *w, float v, int n, int armed) {
  int64_t row = ecs_entity_row(w, ecs_Source_spawn(w));
  ecs_Source_Amount_v(w)[row] = v;
  ecs_Source_Amount_n(w)[row] = n;
  ecs_Source_Amount_seen(w)[row] = -1;
  ecs_Source_Armed_present(w)[row] = armed;
}

int main(void) {
  ecs_world *w = ecs_world_create();
  *ecs_Total_sum(w) = 0;
  *ecs_Total_lo(w) = 100;
  *ecs_Total_count(w) = 0;
  source(w, 1e8f, 5, 1);
  source(w, 1, -3, 1);
  source(w, -1e8f, 7, 1);
  source(w, 1000, -50, 0); // not armed: sends nothing
  ecs_frame(w);
  printf("total: sum %.1f lo %d count %lld\n", *ecs_Total_sum(w),
         *ecs_Total_lo(w), (long long)*ecs_Total_count(w));
  printf("seen:");
  for (int i = 0; i < 4; ++i)
    printf(" %.1f", ecs_Source_Amount_seen(w)[i]);
  printf("\n");
  ecs_world_destroy(w);
  return 0;
}
