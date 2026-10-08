// The host of text_host.ent: runs its schedule with a text of its own,
// and is the extern system that prints what the program made of it.
#include "text_host_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A text as the program takes one: the length, then the bytes.
static const ent_text *text_of(const char *words) {
  static union {
    ent_text text;
    char room[64];
  } held;
  held.text.length = (uint32_t)strlen(words);
  memcpy(held.text.bytes, words, held.text.length + 1);
  return &held.text;
}

void ent_report(ent_world *world, const ent_text *before) {
  const ent_text **words = ent_Note_archetype_Note_words(world);
  const ent_text *title = *ent_Title_value(world);
  printf("%s(%s)\n", before->bytes, title ? title->bytes : "");
  for (int64_t i = 0; i < ent_Note_archetype_count(world); ++i)
    printf("  %s (%u)\n", words[i] ? words[i]->bytes : "",
           words[i] ? (unsigned)words[i]->length : 0u);
}

int main(void) {
  ent_world *world = ent_world_create();
  ent_frame(world, text_of("one"), 1);
  ent_frame(world, text_of("two"), 2);
  // (No address: a text without bytes.)
  ent_frame(world, 0, 3);
  return 0;
}
