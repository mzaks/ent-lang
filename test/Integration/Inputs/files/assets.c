// The C of assets.ent: finds a file as the devices do.
#include "ent_extern.h"
#include "../../../../devices/ent_files.h"

#include <stdio.h>
#include <string.h>

void ent_assets_show(const ent_text62 *file) {
  char name[63], line[128];
  memcpy(name, file->bytes, file->length);
  name[file->length] = 0;
  // (Its bytes from wherever it is: on its own or in the program.)
  struct ent_file held;
  if (!ent_file_open(name, &held)) {
    printf("%s: not there\n", name);
    return;
  }
  size_t length = 0;
  while (length < held.size && length < sizeof line - 1 &&
         held.bytes[length] != '\n')
    ++length;
  memcpy(line, held.bytes, length);
  line[length] = 0;
  ent_file_close(&held);
  printf("%s: %s\n", name, line);
}
