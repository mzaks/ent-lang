// The console device (devices/console.ent): what it does to standard
// output. Compiled with the program, against the declarations generated
// for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <stdio.h>

// Where it writes: standard output, or where devices/ranks.c says a
// partition's lines are kept (what a `for` over distributed entities
// writes, on many ranks).
__attribute__((weak)) FILE *ent_stdout(void) { return stdout; }

void ent_console_put(const ent_text *line) {
  FILE *out = ent_stdout();
  fwrite(line->bytes, 1, line->length, out);
  fputc('\n', out);
}

void ent_console_flush(void) { fflush(ent_stdout()); }
