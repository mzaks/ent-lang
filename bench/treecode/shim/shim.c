/* NEMO's main, parameters and helpers as treecode 1.4 uses them, to build
   it by itself (bench/treecode/run.py): `treecode name=value ..`, each
   name one of `defv`'s, whose value is otherwise its default. */
#include "getparam.h"
#include "mathfns.h"
#include <stdarg.h>
#include <time.h>

extern string defv[];
int nemo_main(void);

#define MAXPARAMS 64
static char *names[MAXPARAMS], *values[MAXPARAMS];
static int given[MAXPARAMS], count;
static string argv0 = "treecode";

static int find(string name) {
  for (int i = 0; i < count; i++)
    if (strcmp(names[i], name) == 0)
      return i;
  error("getparam: no parameter %s\n", name);
  return -1;
}

int main(int argc, char **argv) {
  argv0 = argv[0];
  for (; defv[count]; count++) {
    char *line = strdup(defv[count]);
    char *equal = strchr(line, '=');
    char *end = strchr(line, '\n');
    *equal = 0;
    if (end)
      *end = 0;
    names[count] = line;
    values[count] = equal + 1;
  }
  for (int a = 1; a < argc; a++) {
    char *line = strdup(argv[a]);
    char *equal = strchr(line, '=');
    if (!equal)
      error("treecode: '%s' is not name=value\n", line);
    *equal = 0;
    int i = find(line);
    values[i] = equal + 1;
    given[i] = 1;
  }
  return nemo_main();
}

string getargv0(void) { return argv0; }
string getversion(void) { return getparam("VERSION"); }
string getparam(string name) { return values[find(name)]; }
int getparamstat(string name) { return given[find(name)] ? ARGPARAM : 0; }
int getiparam(string name) { return atoi(getparam(name)); }
bool getbparam(string name) {
  string value = getparam(name);
  return value[0] == 't' || value[0] == 'T' || value[0] == '1';
}
/* (A number, or one over another: "1/32".) */
double getdparam(string name) {
  string value = getparam(name);
  char *slash = strchr(value, '/');
  return slash ? atof(value) / atof(slash + 1) : atof(value);
}

void *allocate(int bytes) {
  void *memory = calloc(1, bytes);
  if (!memory)
    error("allocate: no memory for %d bytes\n", bytes);
  return memory;
}

void error(string format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  exit(1);
}

/* In seconds, where NEMO's is in minutes: treecode's report ("%8.3f")
   then has its times to the ms. */
double cputime(void) { return clock() / (double)CLOCKS_PER_SEC; }

stream stropen(string name, string mode) {
  stream str = fopen(name, mode);
  if (!str)
    error("stropen: cannot open %s\n", name);
  return str;
}
void strclose(stream str) { fclose(str); }
bool strnull(string s) { return s == NULL || s[0] == 0; }

/* Whether `word` is one of the comma-separated words of `options`. */
bool scanopt(string options, string word) {
  size_t length = strlen(word);
  for (string at = options; at && *at;) {
    string end = strchr(at, ',');
    size_t here = end ? (size_t)(end - at) : strlen(at);
    if (here == length && strncmp(at, word, length) == 0)
      return TRUE;
    at = end ? end + 1 : NULL;
  }
  return FALSE;
}

double xrandom(double low, double high) {
  return low + (high - low) * ((double)random()) / 2147483647.0;
}

/* A point at `radius` from the centre, in a direction at random. */
void pickshell(real *vec, int ndim, real radius) {
  real rsq;
  do {
    rsq = 0.0;
    for (int i = 0; i < ndim; i++) {
      vec[i] = xrandom(-1.0, 1.0);
      rsq += vec[i] * vec[i];
    }
  } while (rsq > 1.0);
  real scale = radius / sqrt(rsq);
  for (int i = 0; i < ndim; i++)
    vec[i] *= scale;
}
