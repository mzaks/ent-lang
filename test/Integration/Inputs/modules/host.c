// What a C host of the same program would call.

#include "app_world.h"

void run(ent_world *w) {
  ent_ticker_world_init(w);
  ent_world_init(w);
  ent_frame(w);
}
