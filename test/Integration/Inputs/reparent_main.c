// A host that gives a node another parent between two steps of the scene
// graph (bench/scene/scene.ent, made reactive): the node and what is
// below it are placed again though nothing of theirs has changed.
#include <stdio.h>

#include "scene_world.h"

int main(void) {
  ent_world *w = ent_world_create();
  if (!w)
    return 1;
  // 0 <- 1 <- 3 <- 4, and 0 <- 2.
  ent_entity ids[5];
  int local[5] = {0, 10, 20, 100, 1000};
  int parent[5] = {-1, 0, 0, 1, 3};
  for (int i = 0; i < 5; ++i) {
    ids[i] = ent_Node_spawn(w);
    ent_Node_Local_x(w)[ent_entity_row(w, ids[i])] = local[i];
    ent_Node_World_x(w)[ent_entity_row(w, ids[i])] = 0;
  }
  for (int i = 1; i < 5; ++i)
    if (!ent_Under_connect(w, ids[i], ids[parent[i]]))
      return 1;
  ent_step(w);
  for (int i = 0; i < 5; ++i)
    printf("%d: %d\n", i, ent_Node_World_x(w)[ent_entity_row(w, ids[i])]);
  // 3 goes under 2, with 4 below it.
  if (!ent_Under_connect(w, ids[3], ids[2]))
    return 1;
  ent_step(w);
  for (int i = 0; i < 5; ++i)
    printf("%d: %d\n", i, ent_Node_World_x(w)[ent_entity_row(w, ids[i])]);
  ent_world_destroy(w);
  return 0;
}
