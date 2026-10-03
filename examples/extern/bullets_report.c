// The extern system of examples/bullets_main.ent: prints every gun's
// cooldown and every live bullet, reading the world through the generated
// header, which also declares this function.

#include "bullets_main_world.h"

#include <stdio.h>

void ent_report(ent_world *w) {
  for (int64_t i = 0; i < ent_Gun_count(w); ++i)
    printf("gun %lld: cooldown %.4f\n", (long long)i,
           ent_Gun_Cooldown_seconds(w)[i]);
  int64_t bullets = ent_Position_Velocity_Lifetime_count(w);
  printf("bullets: %lld\n", (long long)bullets);
  for (int64_t i = 0; i < bullets; ++i)
    printf("bullet %lld: x %.4f life %.4f\n", (long long)i,
           ent_Position_Velocity_Lifetime_Position_x(w)[i],
           ent_Position_Velocity_Lifetime_Lifetime_seconds(w)[i]);
}
