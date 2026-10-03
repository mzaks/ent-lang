// The extern system of the module ticker: in C a module's names are
// spelled with `_`.

#include "app_world.h"

#include <stdio.h>

void ent_ticker_show(ent_world *w) {
  printf("ticks %d of %d\n", *ent_ticker_Ticks_count(w), *ent_Limit_value(w));
}
