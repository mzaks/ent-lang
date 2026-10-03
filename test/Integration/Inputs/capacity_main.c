// Spawns one entity per frame into an archetype of capacity 2.
#include "capacity_world.h"

#include <stdio.h>

int main(void) {
  // The message of a failed assert goes through stdout before abort(), and
  // glibc, unlike macOS, does not flush a redirected stdout on abort.
  setvbuf(stdout, NULL, _IOLBF, 0);
  ent_world *w = ent_world_create();
  for (int i = 0; i < 3; ++i) {
    ent_frame(w, (float)i);
    printf("count %lld\n", (long long)ent_A_count(w));
    fflush(stdout);
  }
  return 0;
}
