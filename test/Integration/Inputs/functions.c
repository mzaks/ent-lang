// The C of functions.ent, against the header that has only its functions.
#include "ent_extern.h"

#include <stdio.h>

static int32_t lines;

int64_t ent_mix(int64_t id) { return id * 3 + 1; }
bool ent_is_odd(int64_t id) { return id % 2 != 0; }
bool ent_longer(const ent_text14 *name, int32_t than) {
  return name->length > than;
}

void ent_say(const ent_text30 *line) {
  printf("%.*s\n", line->length, line->bytes);
}

void ent_show(const ent_text14 *name, int64_t id, bool odd) {
  printf("%.*s: %lld%s\n", name->length, name->bytes, (long long)id,
         odd ? " odd" : "");
  ++lines;
}

int32_t ent_shown(void) { return lines; }
