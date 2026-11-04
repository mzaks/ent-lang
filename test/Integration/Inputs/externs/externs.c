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

int32_t ent_mode(void) {
  const char *mode = getenv("CHECK_MODE");
  return mode ? atoi(mode) : 0;
}
void ent_set_scale(float s) { scale = s; }
void ent_say(const ent_text62 *line) {
  printf("%.*s\n", line->length, line->bytes);
}
