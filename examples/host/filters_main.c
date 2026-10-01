// Host for filters.ent: four frames, the third of them paused.

#include "filters_world.h"

#include <stdio.h>

int main(void) {
  ent_world *w = ent_world_create();
  for (int frame = 0; frame < 4; ++frame) {
    *ent_Paused_value(w) = frame == 2;
    ent_frame(w);
    printf("frame %d: clock %lld enemies %d exposed %d elemental %d "
           "shielded %d hp %d\n",
           frame, (long long)*ent_Clock_frame(w), *ent_Stats_enemies(w),
           *ent_Stats_exposed(w), *ent_Stats_elemental(w),
           *ent_Stats_shielded(w), *ent_Stats_hp(w));
  }
  ent_world_destroy(w);
  return 0;
}
