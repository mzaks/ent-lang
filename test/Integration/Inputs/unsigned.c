// The C of the module `unsigned`.
#include "ent_extern.h"
#include <stdio.h>

uint32_t ent_unsigned_rotate(uint32_t bits, uint32_t by) {
  return bits << by | bits >> (32 - by);
}

uint64_t ent_unsigned_widest(void) { return UINT64_MAX; }

uint32_t ent_unsigned_show_bytes(int32_t count, const uint8_t *value,
                                 const uint16_t *at) {
  uint32_t sum = 0;
  for (int32_t i = 0; i < count; i++) {
    printf("  byte %u at %u\n", value[i], at[i]);
    sum += value[i];
  }
  fflush(stdout);
  return sum;
}
