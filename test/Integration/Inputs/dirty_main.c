// A host that gives the world memory that is not zero: every 32-bit word
// is 1, which in the slots of the river's tree would read as an edge of
// the first node, into the second. What a new world must have zeroed is
// zeroed by it, not by luck.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dirty_memalign(void **memory, size_t alignment, size_t bytes) {
  int failed = posix_memalign(memory, alignment, bytes);
  if (!failed)
    for (size_t at = 0; at + 4 <= bytes; at += 4)
      memcpy((char *)*memory + at, &(unsigned){1}, 4);
  return failed;
}
#define posix_memalign dirty_memalign
#include "river_world.h"
#undef posix_memalign

int main(void) {
  ent_world *w = ent_world_create();
  if (!w)
    return 1;
  // sea <- river <- brook, and a lake on its own.
  ent_entity ids[4];
  float rain[4] = {1.0f, 10.0f, 100.0f, 5.0f};
  for (int i = 0; i < 4; ++i) {
    ids[i] = ent_Cell_spawn(w);
    ent_Cell_Node_rain(w)[ent_entity_row(w, ids[i])] = rain[i];
    ent_Cell_Node_flow(w)[ent_entity_row(w, ids[i])] = 0.0f;
  }
  if (!ent_Flows_connect(w, ids[1], ids[0]) ||
      !ent_Flows_connect(w, ids[2], ids[1]))
    return 1;
  ent_step(w, 1.0f);
  // The brook flows into the lake instead.
  ent_divert(w, ids[2], ids[3]);
  ent_step(w, 2.0f);
  for (int i = 0; i < 4; ++i)
    printf("%d: %.0f\n", i, ent_Cell_Node_flow(w)[ent_entity_row(w, ids[i])]);
  ent_world_destroy(w);
  return 0;
}
