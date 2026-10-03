// The extern systems of extern.ent, against the generated header.

#include "extern_world.h"

#include <stdio.h>

void ent_load(ent_world *w, int32_t n, float first) {
  for (int32_t i = 0; i < n; ++i) {
    ent_entity id = ent_Rock_spawn(w);
    ent_Rock_Position_x(w)[ent_entity_row(w, id)] = first + (float)i;
  }
}

void ent_show(ent_world *w, bool verbose, float scale) {
  ++*ent_Console_lines(w);
  printf("frame %d:", *ent_Clock_frame(w));
  if (verbose)
    for (int64_t i = 0; i < ent_Rock_count(w); ++i)
      printf(" %.1f", ent_Rock_Position_x(w)[i] * scale);
  else
    printf(" lines %d", *ent_Console_lines(w));
  printf("\n");
}

void ent_decide(ent_world *w, int32_t limit) {
  *ent_Done_value(w) = *ent_Clock_frame(w) >= limit;
}
