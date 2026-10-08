// The C of assets.ent: finds a file as the devices do.
#include "ent_extern.h"
#include "../../../../devices/ent_files.h"

#include <stdio.h>
#include <string.h>

void ent_assets_show(const ent_text62 *file) {
  char name[63], found[1024], line[128] = "";
  memcpy(name, file->bytes, file->length);
  name[file->length] = 0;
  FILE *from = fopen(ent_file_find(name, found, sizeof found), "r");
  if (!from) {
    printf("%s: not there\n", name);
    return;
  }
  if (fgets(line, sizeof line, from))
    line[strcspn(line, "\n")] = 0;
  fclose(from);
  printf("%s: %s\n", name, line);
}
