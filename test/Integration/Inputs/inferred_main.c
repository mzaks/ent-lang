// Host for inferred.mlir: a unit spawned in frame 0, none in frame 1.

#include "inferred_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  printf("capacity %d\n", ENT_Hull_Shield_CAPACITY);
  for (int frame = 0; frame < 2; ++frame) {
    ent_frame(w, frame == 0);
    printf("frame %d: units %lld hp %.1f shield %d\n", frame,
           (long long)ent_Hull_Shield_count(w), ent_Hull_Shield_Hull_hp(w)[0],
           ent_Hull_Shield_Shield_present(w)[0]);
  }
  ent_world_destroy(w);
  return 0;
}
