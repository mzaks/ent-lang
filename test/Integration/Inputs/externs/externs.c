// The C of externs.ent.
#include "ent_extern.h"

#include <stdio.h>
#include <stdlib.h>

static int32_t calls;
static float scale = 1;

int32_t ent_doubled(int32_t x) { return 2 * x; }
int32_t ent_letters(const ent_text14 *name) { return name->length; }
int32_t ent_counted(int32_t x) { return x + calls++; }
float ent_scaled(int32_t x) { return x * scale; }

int32_t externs_helper(int32_t x);
int32_t ent_hidden(int32_t x) { return externs_helper(x); }
// (helper.c gives the same for the same.)
ENT_SAME_RESULT int32_t ent_shared(int32_t x) { return externs_helper(x); }

// x, by way of the squares below x kept in memory of its own.
int32_t ent_kept(int32_t x) {
  int64_t *squares = malloc(sizeof(int64_t) * (x + 1));
  if (!squares) abort();
  // (volatile: kept, not made away with as LLVM would.)
  volatile int64_t *kept = squares;
  int64_t sum = 0;
  for (int32_t i = 0; i <= x; i++) kept[i] = (int64_t)i * i;
  for (int32_t i = 0; i <= x; i++) sum += kept[i];
  free(squares);
  return sum == (int64_t)x * (x + 1) * (2 * x + 1) / 6 ? x : -1;
}
// x, where it is not below 0.
int32_t ent_checked(int32_t x) {
  if (x < 0) {
    fprintf(stderr, "below 0\n");
    abort();
  }
  return x;
}

int32_t ent_mode(void) {
  const char *mode = getenv("CHECK_MODE");
  return mode ? atoi(mode) : 0;
}
void ent_set_scale(float s) { scale = s; }
void ent_say(const ent_text62 *line) {
  printf("%.*s\n", line->length, line->bytes);
}
