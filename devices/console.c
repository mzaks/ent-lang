// The console device (devices/console.ent): writes every pending line to
// standard output. Compiled with the program, against its generated
// header, which the build names ent_world.h.

#include "ent_world.h"

#include <stdio.h>

void ent_console_write(ent_world *world) {
  int64_t lines = ent_console_Lines_count(world);
  const ent_text126 *line = ent_console_Lines_console_Print_line(world);
  for (int64_t i = 0; i < lines; ++i) {
    fwrite(line[i].bytes, 1, line[i].length, stdout);
    fputc('\n', stdout);
  }
  fflush(stdout);
}
