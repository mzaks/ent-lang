// Host program for examples/bullets.mlir: two guns, eight frames of
// dt = 0.25, then every gun's cooldown and every live bullet.

#include "bullets_world.h"

#include <stdio.h>

enum { GUNS = 2, FRAMES = 8 };

int main(void) {
  ecs_world *w = ecs_world_create();
  if (!w || !ecs_Gun_set_count(w, GUNS)) {
    fprintf(stderr, "could not set up the world\n");
    return 1;
  }
  const float x[GUNS] = {0, 100}, cooldown[GUNS] = {0, 0.5f};
  for (int i = 0; i < GUNS; ++i) {
    ecs_Gun_Position_x(w)[i] = x[i];
    ecs_Gun_Cooldown_seconds(w)[i] = cooldown[i];
    ecs_Gun_Cooldown_period(w)[i] = 1.0f;
  }

  for (int frame = 0; frame < FRAMES; ++frame)
    ecs_frame(w, 0.25f);

  for (int i = 0; i < GUNS; ++i)
    printf("gun %d: cooldown %.4f\n", i, ecs_Gun_Cooldown_seconds(w)[i]);
  printf("bullets: %lld\n", (long long)ecs_Bullet_count(w));
  for (int64_t i = 0; i < ecs_Bullet_count(w); ++i)
    printf("bullet %lld: x %.4f life %.4f\n", (long long)i,
           ecs_Bullet_Position_x(w)[i], ecs_Bullet_Lifetime_seconds(w)[i]);
  ecs_world_destroy(w);
  return 0;
}
