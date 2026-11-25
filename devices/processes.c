// The C of the module `processes` (devices/processes.ent): programs
// started with fork and exec, their output and errors read through pipes
// that never block, their end asked with waitpid(WNOHANG). Written for
// ent-lang.
#include "ent_extern.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__EMSCRIPTEN__)

int64_t ent_processes_process_start(const ent_text *program, const ent_text *arguments,
                                    const ent_text *folder) {
  (void)program, (void)arguments, (void)folder;
  return 0;
}
const ent_text *ent_processes_process_output(int64_t handle) { (void)handle; return ent_text_new(0); }
const ent_text *ent_processes_process_errors(int64_t handle) { (void)handle; return ent_text_new(0); }
int32_t ent_processes_process_status(int64_t handle) { (void)handle; return 1; }
void ent_processes_process_stop(int64_t handle) { (void)handle; }
void ent_processes_process_forget(int64_t handle) { (void)handle; }
const ent_text *ent_processes_program_file(void) { return ent_text_new(0); }

#else

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// A process started: its id, its pipes (-1 once they closed), what was
// read from them and not yet asked for, and its status (-1: running).
typedef struct {
  pid_t pid;
  int out, err;
  char *out_bytes, *err_bytes;
  size_t out_length, err_length;
  int status;
  bool used;
} process;

static process *table;
static int table_room;

static process *process_at(int64_t handle) {
  if (handle <= 0 || handle > table_room || !table[handle - 1].used)
    return NULL;
  return &table[handle - 1];
}

static char *copied(const ent_text *t) {
  size_t n = t ? t->length : 0;
  char *s = malloc(n + 1);
  if (s) {
    if (n)
      memcpy(s, t->bytes, n);
    s[n] = 0;
  }
  return s;
}

// What can be read from a pipe now, added to `bytes`; the pipe closed
// (-1) at its end.
static void drain(int *fd, char **bytes, size_t *length) {
  char chunk[65536];
  while (*fd >= 0) {
    ssize_t n = read(*fd, chunk, sizeof chunk);
    if (n > 0) {
      char *more = realloc(*bytes, *length + (size_t)n);
      if (!more)
        return;
      memcpy(more + *length, chunk, (size_t)n);
      *bytes = more;
      *length += (size_t)n;
    } else if (n == 0) {
      close(*fd);
      *fd = -1;
    } else if (errno == EINTR) {
      continue;
    } else {
      return; // (EAGAIN: nothing more for now.)
    }
  }
}

static void look(process *p) {
  drain(&p->out, &p->out_bytes, &p->out_length);
  drain(&p->err, &p->err_bytes, &p->err_length);
  if (p->status < 0 && p->pid > 0) {
    int status = 0;
    pid_t done = waitpid(p->pid, &status, WNOHANG);
    if (done == p->pid) {
      p->status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
      // (What it wrote last, before it ended.)
      drain(&p->out, &p->out_bytes, &p->out_length);
      drain(&p->err, &p->err_bytes, &p->err_length);
    }
  }
}

int64_t ent_processes_process_start(const ent_text *program, const ent_text *arguments,
                                    const ent_text *folder) {
  char *name = copied(program);
  char *args = copied(arguments);
  char *where = copied(folder);
  if (!name || !args || !where || !name[0]) {
    free(name), free(args), free(where);
    return 0;
  }
  // argv: the program, then each line.
  int count = 1;
  for (char *c = args; *c; ++c)
    if (*c == '\n')
      count++;
  char **argv = calloc((size_t)count + 2, sizeof *argv);
  if (!argv) {
    free(name), free(args), free(where);
    return 0;
  }
  int argc = 0;
  argv[argc++] = name;
  char *line = args;
  while (*line) {
    char *end = strchr(line, '\n');
    if (end)
      *end = 0;
    argv[argc++] = line;
    if (!end)
      break;
    line = end + 1;
  }
  argv[argc] = NULL;
  int out[2] = {-1, -1}, err[2] = {-1, -1}, told[2] = {-1, -1};
  bool piped = pipe(out) == 0 && pipe(err) == 0 && pipe(told) == 0;
  if (piped)
    fcntl(told[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = piped ? fork() : -1;
  if (pid == 0) {
    // (The child: only what is safe between fork and exec.)
    dup2(out[1], 1);
    dup2(err[1], 2);
    close(out[0]), close(out[1]), close(err[0]), close(err[1]), close(told[0]);
    if (where[0] && chdir(where) != 0) {
      int e = errno;
      ssize_t w = write(told[1], &e, sizeof e);
      (void)w;
      _exit(127);
    }
    execvp(name, argv);
    int e = errno;
    ssize_t w = write(told[1], &e, sizeof e);
    (void)w;
    _exit(127);
  }
  if (out[1] >= 0)
    close(out[1]);
  if (err[1] >= 0)
    close(err[1]);
  if (told[1] >= 0)
    close(told[1]);
  // (Whether exec failed: the child says so before the pipe closes.)
  int why = 0;
  bool failed = pid < 0;
  if (pid > 0 && told[0] >= 0) {
    ssize_t n;
    do {
      n = read(told[0], &why, sizeof why);
    } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof why)
      failed = true;
  }
  if (told[0] >= 0)
    close(told[0]);
  free(argv);
  free(name), free(args), free(where);
  if (failed) {
    if (pid > 0)
      waitpid(pid, NULL, 0);
    if (out[0] >= 0)
      close(out[0]);
    if (err[0] >= 0)
      close(err[0]);
    return 0;
  }
  fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
  fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL) | O_NONBLOCK);
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  fcntl(err[0], F_SETFD, FD_CLOEXEC);
  int slot = -1;
  for (int k = 0; k < table_room && slot < 0; ++k)
    if (!table[k].used)
      slot = k;
  if (slot < 0) {
    int room = table_room ? table_room * 2 : 16;
    process *more = realloc(table, sizeof *table * (size_t)room);
    if (!more) {
      kill(pid, SIGTERM);
      waitpid(pid, NULL, 0);
      close(out[0]), close(err[0]);
      return 0;
    }
    memset(more + table_room, 0, sizeof *table * (size_t)(room - table_room));
    slot = table_room;
    table = more;
    table_room = room;
  }
  process *p = &table[slot];
  memset(p, 0, sizeof *p);
  p->pid = pid;
  p->out = out[0];
  p->err = err[0];
  p->status = -1;
  p->used = true;
  return slot + 1;
}

static const ent_text *taken(char **bytes, size_t *length) {
  ent_text *t = ent_text_new((uint32_t)*length);
  if (t && *length)
    memcpy(t->bytes, *bytes, *length);
  free(*bytes);
  *bytes = NULL;
  *length = 0;
  return t;
}

const ent_text *ent_processes_process_output(int64_t handle) {
  process *p = process_at(handle);
  if (!p)
    return ent_text_new(0);
  look(p);
  return taken(&p->out_bytes, &p->out_length);
}

const ent_text *ent_processes_process_errors(int64_t handle) {
  process *p = process_at(handle);
  if (!p)
    return ent_text_new(0);
  look(p);
  return taken(&p->err_bytes, &p->err_length);
}

int32_t ent_processes_process_status(int64_t handle) {
  process *p = process_at(handle);
  if (!p)
    return 1;
  look(p);
  return p->status;
}

void ent_processes_process_stop(int64_t handle) {
  process *p = process_at(handle);
  if (p && p->status < 0)
    kill(p->pid, SIGTERM);
}

void ent_processes_process_forget(int64_t handle) {
  process *p = process_at(handle);
  if (!p)
    return;
  if (p->status < 0) {
    kill(p->pid, SIGTERM);
    waitpid(p->pid, NULL, 0);
  }
  if (p->out >= 0)
    close(p->out);
  if (p->err >= 0)
    close(p->err);
  free(p->out_bytes);
  free(p->err_bytes);
  memset(p, 0, sizeof *p);
}

const ent_text *ent_processes_program_file(void) {
  char path[PATH_MAX + 1];
  size_t length = 0;
#if defined(__APPLE__)
  uint32_t size = sizeof path;
  char raw[PATH_MAX + 1];
  if (_NSGetExecutablePath(raw, &size) == 0 && realpath(raw, path))
    length = strlen(path);
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", path, PATH_MAX);
  if (n > 0)
    length = (size_t)n;
#endif
  ent_text *t = ent_text_new((uint32_t)length);
  if (t && length)
    memcpy(t->bytes, path, length);
  return t;
}

#endif
