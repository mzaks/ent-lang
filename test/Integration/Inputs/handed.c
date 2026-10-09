// The C of the module `handed`: a buffer comes as how many rows it has
// and each field's values, one for each row.
#include "ent_extern.h"
#include <stdio.h>

int32_t ent_handed_show(int32_t count, const int32_t *kind, const float *x,
                        const bool *bright, float scale) {
  for (int32_t i = 0; i < count; i++)
    printf("  shape %d: kind %d x %.1f%s\n", i, kind[i], x[i] * scale,
           bright[i] ? " bright" : "");
  return count;
}
