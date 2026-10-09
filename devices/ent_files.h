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

// The path of the file `name`: `name` itself where that is one, or where
// it is nowhere; else written into `found`, which holds `size` bytes.
static const char *ent_file_find(const char *name, char *found, size_t size) {
  if (!*name || *name == '/' || ent_file_is_there(name))
    return name;
  if (ent_file_among(getenv("ENT_PROGRAM_DIR"), name, found, size))
    return found;
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
        return found;
    }
  }
  if (ent_file_among(ent_program_folders, name, found, size))
    return found;
  return name;
}

#endif // ENT_FILES_H
