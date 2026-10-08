// The extern system of text_lines.ent: a text column is an array of structs
// holding a length and the bytes. (The archetype the compiler inferred for
// the lines is named after its component, which has the name already.)

#include "text_world.h"

#include <stdio.h>

void ent_print(ent_world *w) {
  for (int64_t i = 0; i < ent_Line_archetype_count(w); ++i) {
    ent_text62 line = ent_Line_archetype_Line_text(w)[i];
    printf("[%.*s]\n", (int)line.length, line.bytes);
  }
}
