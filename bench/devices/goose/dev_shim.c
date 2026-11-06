// bench/devices' Goose (dev.goose, made into C by
// examples/goose/build.sh) called under the names ent calls: buffers made
// into Goose's slices. ent_extern.h makes a definition that differs from
// dev.ent's declaration an error here.
#include "ent_extern.h"

#include "dev_goose.c"

__attribute__((constructor)) static void dev_start(void) { goose_init(0, 0); }

uint64_t ent_dev_mix(uint64_t x) { return goose_mix(x); }

int64_t ent_dev_square_sum(int32_t n) { return goose_square_sum(n); }

uint64_t ent_dev_median16(uint64_t seed) { return goose_median16(seed); }

int64_t ent_dev_parse(int32_t length, const uint8_t *text, int32_t *count,
                      int32_t room, int64_t *into) {
  int64_t found = goose_parse((sl_u8){(uint8_t *)text, length},
                              (sl_i64){into, room});
  *count = found >= 0 ? (int32_t)found : found == -2 ? room : 0;
  return found;
}

int64_t ent_dev_median(int32_t n, const int64_t *values, int32_t *count,
                       int32_t room, int64_t *scratch) {
  if (n > room) n = room;
  *count = n;
  return goose_median((sl_i64){(int64_t *)values, n},
                      (sl_i64){scratch, n});
}

void ent_dev_blur(int32_t n, const uint8_t *from, int32_t *count,
                  int32_t room, uint8_t *to, int32_t width) {
  if (n > room) n = room;
  goose_blur((sl_u8){(uint8_t *)from, n}, (sl_u8){to, n}, width);
  *count = n / width * width;
}
