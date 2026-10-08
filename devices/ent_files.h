// Where a file is that a program names (a font, a picture, a sound): for
// the devices' C.
//
// A name that is not a whole path is looked for as it is, from where the
// program was started; then in the folder of the program's source and
// in those of the modules it imports, when it was started with `ent run`
// (which says where they are, in ENT_PROGRAM_DIR); then in the folder
// the program itself is in, as one built with `ent build` and kept with
// its files.
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

static int ent_file_is_there(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0;
  fclose(file);
  return 1;
}

// The path of the file `name`: `name` itself where that is one, or where
// it is nowhere; else written into `found`, which holds `size` bytes.
static const char *ent_file_find(const char *name, char *found, size_t size) {
  if (!*name || *name == '/' || ent_file_is_there(name))
    return name;
  // (The program's folder first, then those of the modules it imports.)
  const char *sources = getenv("ENT_PROGRAM_DIR");
  while (sources && *sources) {
    size_t length = strcspn(sources, ENT_FILES_BETWEEN);
    if (length &&
        snprintf(found, size, "%.*s/%s", (int)length, sources, name) <
            (int)size &&
        ent_file_is_there(found))
      return found;
    sources += length;
    if (*sources)
      ++sources;
  }
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
  return name;
}

#endif // ENT_FILES_H
