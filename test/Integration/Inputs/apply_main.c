// Host for apply.mlir: one sink, one gate without a Total, six sources.

#include "apply_world.h"

#include <stdio.h>

static void source(ent_world *w, ent_entity target, float v, int n,
                   int armed) {
  int64_t row = ent_entity_row(w, ent_Source_spawn(w));
  ent_Source_Amount_v(w)[row] = v;
  ent_Source_Amount_n(w)[row] = n;
  ent_Source_Amount_seen(w)[row] = -1;
  ent_Source_Target_entity(w)[row] = target;
  ent_Source_Armed_present(w)[row] = armed;
}

int main(void) {
  ent_world *w = ent_world_create();
  ent_entity sink = ent_Sink_spawn(w);
  ent_Sink_Total_sum(w)[0] = 0;
  ent_Sink_Total_lo(w)[0] = 100;
  ent_Sink_Total_hi(w)[0] = 0;
  ent_entity gate = ent_Gate_spawn(w);
  ent_Gate_Total_present(w)[0] = 0;
  ent_Gate_Total_sum(w)[0] = 0;

  source(w, sink, 1e8f, 5, 1);
  source(w, sink, 1, -3, 1);
  source(w, sink, -1e8f, 7, 1);
  source(w, sink, 1000, 50, 0); // not armed: sends nothing
  source(w, gate, 5, 9, 1);     // the gate has no Total: dropped
  source(w, sink + 1, 5, 9, 1); // row 1 of Sink is empty: dropped
  ent_frame(w);

  printf("sink: sum %.1f lo %d hi %d\n", ent_Sink_Total_sum(w)[0],
         ent_Sink_Total_lo(w)[0], ent_Sink_Total_hi(w)[0]);
  printf("gate: sum %.1f\n", ent_Gate_Total_sum(w)[0]);
  printf("seen:");
  for (int i = 0; i < 6; ++i)
    printf(" %.1f", ent_Source_Amount_seen(w)[i]);
  printf("\n");
  ent_world_destroy(w);
  return 0;
}
