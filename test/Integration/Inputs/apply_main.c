// Host for apply.mlir: one sink, one gate without a Total, six sources.

#include "apply_world.h"

#include <stdio.h>

static void source(ecs_world *w, ecs_entity target, float v, int n,
                   int armed) {
  int64_t row = ecs_entity_row(w, ecs_Source_spawn(w));
  ecs_Source_Amount_v(w)[row] = v;
  ecs_Source_Amount_n(w)[row] = n;
  ecs_Source_Amount_seen(w)[row] = -1;
  ecs_Source_Target_entity(w)[row] = target;
  ecs_Source_Armed_present(w)[row] = armed;
}

int main(void) {
  ecs_world *w = ecs_world_create();
  ecs_entity sink = ecs_Sink_spawn(w);
  ecs_Sink_Total_sum(w)[0] = 0;
  ecs_Sink_Total_lo(w)[0] = 100;
  ecs_Sink_Total_hi(w)[0] = 0;
  ecs_entity gate = ecs_Gate_spawn(w);
  ecs_Gate_Total_present(w)[0] = 0;
  ecs_Gate_Total_sum(w)[0] = 0;

  source(w, sink, 1e8f, 5, 1);
  source(w, sink, 1, -3, 1);
  source(w, sink, -1e8f, 7, 1);
  source(w, sink, 1000, 50, 0); // not armed: sends nothing
  source(w, gate, 5, 9, 1);     // the gate has no Total: dropped
  source(w, sink + 1, 5, 9, 1); // row 1 of Sink is empty: dropped
  ecs_frame(w);

  printf("sink: sum %.1f lo %d hi %d\n", ecs_Sink_Total_sum(w)[0],
         ecs_Sink_Total_lo(w)[0], ecs_Sink_Total_hi(w)[0]);
  printf("gate: sum %.1f\n", ecs_Gate_Total_sum(w)[0]);
  printf("seen:");
  for (int i = 0; i < 6; ++i)
    printf(" %.1f", ecs_Source_Amount_seen(w)[i]);
  printf("\n");
  ecs_world_destroy(w);
  return 0;
}
