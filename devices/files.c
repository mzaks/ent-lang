// The C of the module `files` (devices/files.ent).
#include "ent_extern.h"
#include "ent_files.h"

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
