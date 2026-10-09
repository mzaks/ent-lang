// Where a file is that a program names (a font, a picture, a sound): for
// the devices' C.
//
// A name that is not a whole path is looked for as it is, from where the
// program was started; then in the folder of the program's source and
// in those of the modules it imports, when it was started with `ent run`
// (which says where they are, in ENT_PROGRAM_DIR); then in the folder
// the program itself is in, as one built with `ent build` and kept with
// its files; then where the program's source and its modules' were when
// it was built, which a built program knows (so it runs on the machine
// it was built on without its files being brought to it).
//
// A file the program declares (`asset "name"`) may also be in the
// program itself (`ent build --embed`): `ent_file_open` gives a file's
// bytes from wherever it is, the ones in the program where the file is
// not next to where it was started, to its source or to itself.
#ifndef ENT_FILES_H
#define ENT_FILES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

// (What is between the folders of a list of them.)
#ifdef _WIN32
#define ENT_FILES_BETWEEN ";"
#else
#define ENT_FILES_BETWEEN ":"
#endif

// The folders a program was built from, as `ent` writes them into it;
// none in a program that was built another way.
#if defined(__GNUC__)
__attribute__((weak))
#endif
const char ent_program_folders[] = "";

// The files that are in the program itself: for each its name as the
// program writes it, its bytes and how many they are; after the last, one
// without a name.
struct ent_asset {
  const char *name;
  const unsigned char *bytes;
  unsigned long size;
};
#if defined(__GNUC__)
__attribute__((weak))
#endif
const struct ent_asset ent_program_assets[] = {{0, 0, 0}};

static int ent_file_is_there(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0;
  fclose(file);
  return 1;
}

// Whether `name` is in one of the folders of a list of them: the program's
// first, then those of the modules it imports. Its path is in `found`.
static int ent_file_among(const char *folders, const char *name, char *found,
                          size_t size) {
  while (folders && *folders) {
    size_t length = strcspn(folders, ENT_FILES_BETWEEN);
    if (length &&
        snprintf(found, size, "%.*s/%s", (int)length, folders, name) <
            (int)size &&
        ent_file_is_there(found))
      return 1;
    folders += length;
    if (*folders)
      ++folders;
  }
  return 0;
}

// Whether `name` is where the program was started, where `ent run` says
// its source is, or where the program itself is. Its path is in `found`.
static int ent_file_near(const char *name, char *found, size_t size) {
  if (ent_file_is_there(name))
    return snprintf(found, size, "%s", name) < (int)size;
  if (ent_file_among(getenv("ENT_PROGRAM_DIR"), name, found, size))
    return 1;
  char program[1024];
  long length = -1;
#if defined(__APPLE__)
  uint32_t room = sizeof program;
  if (_NSGetExecutablePath(program, &room) == 0)
    length = (long)strlen(program);
#elif defined(__linux__)
  length = (long)readlink("/proc/self/exe", program, sizeof program - 1);
#endif
  if (length > 0) {
    program[length] = 0;
    char *slash = strrchr(program, '/');
    if (slash) {
      *slash = 0;
      if (snprintf(found, size, "%s/%s", program, name) < (int)size &&
          ent_file_is_there(found))
        return 1;
    }
  }
  return 0;
}

// The path of the file `name`: `name` itself where that is one, or where
// it is nowhere; else written into `found`, which holds `size` bytes.
// (Of a file on its own; one in the program has no path.)
static const char *ent_file_find(const char *name, char *found, size_t size) {
  if (!*name || *name == '/')
    return name;
  if (ent_file_near(name, found, size) ||
      ent_file_among(ent_program_folders, name, found, size))
    return found;
  return name;
}

// A file's bytes. (`own` is what was read of a file on its own, which
// `ent_file_close` gives back; the bytes of one in the program stay.)
struct ent_file {
  const unsigned char *bytes;
  size_t size;
  void *own;
};

static int ent_file_read(const char *path, struct ent_file *file) {
  FILE *from = fopen(path, "rb");
  if (!from)
    return 0;
  long size = fseek(from, 0, SEEK_END) == 0 ? ftell(from) : -1;
  unsigned char *bytes = size >= 0 ? (unsigned char *)malloc(size + 1) : 0;
  if (!bytes || fseek(from, 0, SEEK_SET) != 0 ||
      fread(bytes, 1, size, from) != (size_t)size) {
    free(bytes);
    fclose(from);
    return 0;
  }
  fclose(from);
  bytes[size] = 0;
  file->bytes = bytes;
  file->size = size;
  file->own = bytes;
  return 1;
}

// The bytes of the file `name`, from wherever it is; 0 if nowhere.
static int ent_file_open(const char *name, struct ent_file *file) {
  char found[1024];
  file->bytes = 0;
  file->size = 0;
  file->own = 0;
  if (!*name)
    return 0;
  if (*name == '/')
    return ent_file_read(name, file);
  if (ent_file_near(name, found, sizeof found))
    return ent_file_read(found, file);
  for (const struct ent_asset *asset = ent_program_assets; asset->name;
       ++asset)
    if (!strcmp(asset->name, name)) {
      file->bytes = asset->bytes;
      file->size = asset->size;
      return 1;
    }
  if (ent_file_among(ent_program_folders, name, found, sizeof found))
    return ent_file_read(found, file);
  return 0;
}

static void ent_file_close(struct ent_file *file) {
  free(file->own);
  file->bytes = 0;
  file->size = 0;
  file->own = 0;
}

#endif // ENT_FILES_H
