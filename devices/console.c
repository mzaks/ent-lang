// The console device (devices/console.ent): what it does to standard
// output. Compiled with the program, against the declarations generated
// for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <stdio.h>
#include <string.h>

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

void ent_console_put_error(const ent_text *line) {
  fflush(ent_stdout());
  if (line)
    fwrite(line->bytes, 1, line->length, stderr);
  fputc('\n', stderr);
}

// The words the program was started with, kept as it starts: C libraries
// give `main`'s argc and argv to functions run before it (glibc, macOS);
// Windows keeps them in __argc and __argv.
static int ent_console_argc;
static char **ent_console_argv;
#if defined(_WIN32)
#include <stdlib.h>
static void ent_console_take_arguments(void) {
  if (!ent_console_argv) {
    ent_console_argc = __argc;
    ent_console_argv = __argv;
  }
}
#elif defined(__EMSCRIPTEN__)
static void ent_console_take_arguments(void) {}
#else
__attribute__((constructor)) static void ent_console_started(int argc,
                                                             char **argv,
                                                             char **envp) {
  (void)envp;
  ent_console_argc = argc;
  ent_console_argv = argv;
}
static void ent_console_take_arguments(void) {}
#endif

int32_t ent_console_argument_count(void) {
  ent_console_take_arguments();
  return ent_console_argc > 1 ? ent_console_argc - 1 : 0;
}

const ent_text *ent_console_argument(int32_t n) {
  ent_console_take_arguments();
  if (n < 0 || n + 1 >= ent_console_argc || !ent_console_argv)
    return 0;
  const char *word = ent_console_argv[n + 1];
  size_t length = strlen(word);
  if (length == 0)
    return 0;
  ent_text *made = ent_text_new((uint32_t)length);
  memcpy(made->bytes, word, length);
  return made;
}

// The status `main` gives back (the program asks for it at its end).
static int32_t ent_console_status;
void ent_console_exit_status(int32_t code) { ent_console_status = code; }
int32_t ent_console_exit_code(void) { return ent_console_status; }
