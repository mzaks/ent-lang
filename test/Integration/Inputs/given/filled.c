// The C of the module `filled`: a buffer to change comes as where its
// count is, how many rows it holds, and each field's values.
#include "ent_extern.h"

void ent_filled_fill_bytes(int32_t *count, int32_t capacity, uint8_t *value,
                           int32_t from) {
  // Ten rows after those it has, or as many as there is room for.
  int32_t rows = *count;
  for (int32_t i = 0; i < 10 && rows < capacity; i++)
    value[rows++] = (uint8_t)(from + i);
  *count = rows;
}

int32_t ent_filled_sum_bytes(int32_t count, const uint8_t *value) {
  int32_t sum = 0;
  for (int32_t i = 0; i < count; i++)
    sum += value[i];
  return sum;
}
