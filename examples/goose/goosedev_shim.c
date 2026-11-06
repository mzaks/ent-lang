// The device goosedev (goosedev.ent) as ent calls it: the Goose of
// goosedev.goose made into C (goosedev_goose.c, by build.sh) and its
// exports called under the names the program calls, with what ent hands
// (a buffer as how many rows and one pointer per field) made into Goose's
// slices. ent_extern.h, ent's declarations, makes a definition that
// differs from them an error here.
//
// (Not goosedev.c: tools/ent would build that as the module's own C.)
#include "ent_extern.h"

#include "goosedev_goose.c"

// Goose's runtime made ready before main, as a Goose program's own main
// would: on the process's main thread, where the program's procs are
// called (docs/syntax.md, Modules).
__attribute__((constructor)) static void goosedev_start(void) {
  goose_init(0, 0);
}

uint64_t ent_goosedev_mix(uint64_t x) { return goose_mix(x); }

int64_t ent_goosedev_square_sum(int32_t n) { return goose_square_sum(n); }

// (`into` as how many rows it has and is left with, how many it holds,
// and where its values are: those Goose wrote are its rows, all it holds
// where they did not fit, none where the text was no list.)
int64_t ent_goosedev_parse(int32_t length, const uint8_t *text,
                           int32_t *count, int32_t room, int64_t *into) {
  sl_u8 bytes = {(uint8_t *)text, length};
  sl_i64 values = {into, room};
  int64_t found = goose_parse(bytes, values);
  *count = found >= 0 ? (int32_t)found : found == -2 ? room : 0;
  return found;
}

int64_t ent_goosedev_median(int32_t count, const int64_t *values) {
  sl_i64 them = {(int64_t *)values, count};
  return goose_median(them);
}
