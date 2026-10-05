// The console device (devices/console.ent): what it does to standard
// output. Compiled with the program, against the declarations generated
// for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <stdio.h>

void ent_console_put(const ent_text126 *line) {
  fwrite(line->bytes, 1, line->length, stdout);
  fputc('\n', stdout);
}

void ent_console_flush(void) { fflush(stdout); }
