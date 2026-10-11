// The C of the module `files` (devices/files.ent).
#include "ent_extern.h"
#include "ent_files.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

int32_t ent_files_read_file(const ent_text126 *name, int32_t *count,
                            int32_t capacity, uint8_t *value) {
  char path[127];
  struct ent_file file;
  memcpy(path, name->bytes, name->length);
  path[name->length] = 0;
  *count = 0;
  if (!ent_file_open(path, &file))
    return -1;
  size_t kept = file.size < (size_t)capacity ? file.size : (size_t)capacity;
  memcpy(value, file.bytes, kept);
  *count = (int32_t)kept;
  int32_t size = file.size > INT32_MAX ? INT32_MAX : (int32_t)file.size;
  ent_file_close(&file);
  return size;
}

bool ent_files_write_file(const ent_text126 *name, int32_t count,
                          const uint8_t *value) {
  char path[127];
  memcpy(path, name->bytes, name->length);
  path[name->length] = 0;
  FILE *to = fopen(path, "wb");
  if (!to)
    return false;
  bool written = fwrite(value, 1, (size_t)count, to) == (size_t)count;
  return fclose(to) == 0 && written;
}

bool ent_files_write_bytes(const ent_text126 *name, int32_t count,
                           const uint8_t *value, int32_t first, int32_t wanted,
                           bool append) {
  char path[127];
  if (first < 0 || wanted < 0 || (int64_t)first + wanted > count)
    return false;
  memcpy(path, name->bytes, name->length);
  path[name->length] = 0;
  FILE *to = fopen(path, append ? "ab" : "wb");
  if (!to)
    return false;
  bool written = fwrite(value + first, 1, (size_t)wanted, to) == (size_t)wanted;
  return fclose(to) == 0 && written;
}

int32_t ent_files_read_bytes(const ent_text126 *name, int32_t *count,
                             int32_t capacity, uint8_t *value, int32_t first,
                             int32_t wanted) {
  char path[127];
  (void)capacity;
  if (first < 0 || wanted < 0 || (int64_t)first + wanted > *count)
    return -1;
  memcpy(path, name->bytes, name->length);
  path[name->length] = 0;
  // (As `write_bytes` writes it: where the program was started, or where
  // a whole path says; not looked for elsewhere.)
  FILE *from = fopen(path, "rb");
  if (!from)
    return -1;
  size_t got = fread(value + first, 1, (size_t)wanted, from);
  fclose(from);
  return (int32_t)got;
}

static int ent_files_by_name(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int32_t ent_files_list_files(const ent_text126 *folder, int32_t *count,
                             int32_t capacity, uint8_t *value) {
  char path[127];
  memcpy(path, folder->bytes, folder->length);
  path[folder->length] = 0;
  *count = 0;
  DIR *open = opendir(path);
  if (!open)
    return -1;
  char **names = 0;
  int32_t found = 0, room = 0;
  for (struct dirent *entry; (entry = readdir(open));) {
    if (entry->d_name[0] == '.')
      continue;
    if (found == room) {
      room = room ? room * 2 : 64;
      names = realloc(names, sizeof(char *) * (size_t)room);
    }
    names[found++] = strdup(entry->d_name);
  }
  closedir(open);
  qsort(names, (size_t)found, sizeof(char *), ent_files_by_name);
  int32_t at = 0;
  for (int32_t i = 0; i < found; i++) {
    size_t length = strlen(names[i]);
    if (at + (int64_t)length + 1 <= capacity) {
      memcpy(value + at, names[i], length);
      value[at + length] = '\n';
      at += (int32_t)length + 1;
    }
    free(names[i]);
  }
  free(names);
  *count = at;
  return found;
}
