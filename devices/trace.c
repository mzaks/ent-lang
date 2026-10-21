// What a program built with `ent build --trace` calls (the ent-trace pass):
// the time of each system, schedule and sort of a relation, and of each
// parallel loop, where it is entered and left, and each thread's own time
// in it. Each thread keeps what it timed in a list of its own (a lock only
// where a thread times its first). When the program ends they are written
// as a trace in Chrome's JSON (ENT_TRACE says where, `%p` the process's
// number; else `program.trace.json` where it runs), which ui.perfetto.dev
// or chrome://tracing shows a line for each thread of, and where the time
// went is said on stderr: of each site, the time on its own (without what
// it called that was timed too), and of a loop how much of its time the
// threads were working.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { ENT_TRACE_SPAN, ENT_TRACE_LOOP, ENT_TRACE_WORK };

typedef struct {
  int64_t begin, end;
  int32_t site;
  int32_t kind;
} ent_trace_event;

// (What one thread timed; `open`: where what it is in began, and what.)
typedef struct {
  ent_trace_event *events;
  int64_t count, room;
  int64_t open[64];
  int32_t opened[64];
  int depth;
  int number;
} ent_trace_thread;

static pthread_mutex_t ent_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static ent_trace_thread **ent_trace_threads;
static int ent_trace_thread_count, ent_trace_thread_room;
static const char **ent_trace_names;
static int64_t ent_trace_name_count;
static int64_t ent_trace_started;
static _Thread_local ent_trace_thread *ent_trace_mine;

int64_t ent_trace_now(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static ent_trace_thread *ent_trace_this(void) {
  if (ent_trace_mine)
    return ent_trace_mine;
  ent_trace_thread *made = calloc(1, sizeof *made);
  pthread_mutex_lock(&ent_trace_lock);
  if (ent_trace_thread_count == ent_trace_thread_room) {
    ent_trace_thread_room =
        ent_trace_thread_room ? 2 * ent_trace_thread_room : 64;
    ent_trace_threads = realloc(
        ent_trace_threads, ent_trace_thread_room * sizeof *ent_trace_threads);
  }
  made->number = ent_trace_thread_count;
  ent_trace_threads[ent_trace_thread_count++] = made;
  pthread_mutex_unlock(&ent_trace_lock);
  return ent_trace_mine = made;
}

static void ent_trace_add(ent_trace_thread *thread, int32_t site, int32_t kind,
                          int64_t begin, int64_t end) {
  if (thread->count == thread->room) {
    thread->room = thread->room ? 2 * thread->room : 4096;
    thread->events =
        realloc(thread->events, thread->room * sizeof *thread->events);
    if (!thread->events) {
      fprintf(stderr, "ent: no memory for the trace\n");
      abort();
    }
  }
  thread->events[thread->count++] =
      (ent_trace_event){.begin = begin, .end = end, .site = site, .kind = kind};
}

static void ent_trace_open(int64_t site) {
  ent_trace_thread *thread = ent_trace_this();
  if (thread->depth < 64) {
    thread->open[thread->depth] = ent_trace_now();
    thread->opened[thread->depth] = (int32_t)site;
  }
  thread->depth += 1;
}
static void ent_trace_close(int64_t site, int32_t kind) {
  int64_t now = ent_trace_now();
  ent_trace_thread *thread = ent_trace_this();
  thread->depth -= 1;
  if (thread->depth < 64)
    ent_trace_add(thread, (int32_t)site, kind, thread->open[thread->depth],
                  now);
}

void ent_trace_begin(int64_t site) { ent_trace_open(site); }
void ent_trace_end(int64_t site) { ent_trace_close(site, ENT_TRACE_SPAN); }
void ent_trace_loop_begin(int64_t site) { ent_trace_open(site); }
void ent_trace_loop_end(int64_t site) { ent_trace_close(site, ENT_TRACE_LOOP); }
void ent_trace_work(int64_t site, int64_t begin) {
  ent_trace_add(ent_trace_this(), (int32_t)site, ENT_TRACE_WORK, begin,
                ent_trace_now());
}

void ent_trace_name(int64_t site, const char *name) {
  if (site >= ent_trace_name_count) {
    int64_t count = site + 1;
    ent_trace_names = realloc(ent_trace_names, count * sizeof(char *));
    for (int64_t k = ent_trace_name_count; k < count; ++k)
      ent_trace_names[k] = "?";
    ent_trace_name_count = count;
  }
  ent_trace_names[site] = name;
}
static const char *ent_trace_name_of(int32_t site) {
  return site < ent_trace_name_count ? ent_trace_names[site] : "?";
}

static void ent_trace_json_text(FILE *out, const char *text) {
  fputc('"', out);
  for (; *text; ++text) {
    if (*text == '"' || *text == '\\')
      fputc('\\', out);
    fputc(*text, out);
  }
  fputc('"', out);
}

static void ent_trace_write(const char *path) {
  FILE *out = fopen(path, "w");
  if (!out) {
    fprintf(stderr, "ent: cannot write the trace to '%s'\n", path);
    return;
  }
  fprintf(out, "{\"traceEvents\":[\n");
  int first = 1;
  for (int t = 0; t < ent_trace_thread_count; ++t) {
    ent_trace_thread *thread = ent_trace_threads[t];
    for (int64_t k = 0; k < thread->count; ++k) {
      ent_trace_event *event = &thread->events[k];
      fprintf(out, "%s{\"name\":", first ? "" : ",\n");
      first = 0;
      ent_trace_json_text(out, event->kind == ENT_TRACE_WORK
                                   ? "work"
                                   : ent_trace_name_of(event->site));
      fprintf(out,
              ",\"cat\":\"%s\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,"
              "\"pid\":1,\"tid\":%d",
              event->kind == ENT_TRACE_SPAN   ? "span"
              : event->kind == ENT_TRACE_LOOP ? "loop"
                                              : "work",
              (event->begin - ent_trace_started) / 1e3,
              (event->end - event->begin) / 1e3, thread->number);
      if (event->kind == ENT_TRACE_WORK) {
        fprintf(out, ",\"args\":{\"loop\":");
        ent_trace_json_text(out, ent_trace_name_of(event->site));
        fputc('}', out);
      }
      fputc('}', out);
    }
  }
  fprintf(out, "\n]}\n");
  fclose(out);
}

// What is said of each site.
typedef struct {
  int64_t count, total, own, work;
  int32_t site, kind;
} ent_trace_sum;

static int ent_trace_by_own(const void *a, const void *b) {
  int64_t x = ((const ent_trace_sum *)a)->own;
  int64_t y = ((const ent_trace_sum *)b)->own;
  return x < y ? 1 : x > y ? -1 : 0;
}
static int ent_trace_by_begin(const void *a, const void *b) {
  const ent_trace_event *x = a, *y = b;
  if (x->begin != y->begin)
    return x->begin < y->begin ? -1 : 1;
  return x->end > y->end ? -1 : x->end < y->end ? 1 : 0;
}

static void ent_trace_say(int64_t ended) {
  ent_trace_sum *sums = calloc(ent_trace_name_count + 1, sizeof *sums);
  for (int64_t k = 0; k <= ent_trace_name_count; ++k)
    sums[k].site = (int32_t)k;
  int workers = 0;
  for (int t = 0; t < ent_trace_thread_count; ++t) {
    ent_trace_thread *thread = ent_trace_threads[t];
    int worked = 0;
    // (Spans and loops in the order they began, the outer first: what
    // each one's own time is, without those inside it.)
    ent_trace_event *nested = malloc((thread->count + 1) * sizeof *nested);
    int64_t many = 0;
    for (int64_t k = 0; k < thread->count; ++k) {
      ent_trace_event *event = &thread->events[k];
      int32_t site = event->site < ent_trace_name_count
                         ? event->site
                         : (int32_t)ent_trace_name_count;
      if (event->kind == ENT_TRACE_WORK) {
        sums[site].work += event->end - event->begin;
        worked = 1;
        continue;
      }
      sums[site].count += 1;
      sums[site].kind = event->kind;
      sums[site].total += event->end - event->begin;
      sums[site].own += event->end - event->begin;
      nested[many++] = *event;
    }
    qsort(nested, many, sizeof *nested, ent_trace_by_begin);
    int64_t stack[64];
    int depth = 0, loops = 0;
    // (Of a thread that runs the program: its time in parallel loops.)
    int64_t parallel = 0;
    for (int64_t k = 0; k < many; ++k) {
      while (depth > 0 && nested[stack[depth - 1]].end <= nested[k].begin) {
        depth -= 1;
        loops -= nested[stack[depth]].kind == ENT_TRACE_LOOP;
      }
      if (depth > 0) {
        int32_t outer = nested[stack[depth - 1]].site;
        if (outer >= ent_trace_name_count)
          outer = (int32_t)ent_trace_name_count;
        sums[outer].own -= nested[k].end - nested[k].begin;
      }
      if (nested[k].kind == ENT_TRACE_LOOP && loops == 0)
        parallel += nested[k].end - nested[k].begin;
      if (depth < 64) {
        stack[depth++] = k;
        loops += nested[k].kind == ENT_TRACE_LOOP;
      }
    }
    if (many > 0) {
      double all = (ended - ent_trace_started) / 1e9;
      fprintf(stderr,
              "ent: thread %d ran the program: %.3f s in parallel loops, "
              "%.3f s on its own (of %.3f s)\n",
              thread->number, parallel / 1e9, all - parallel / 1e9, all);
    }
    free(nested);
    workers += worked;
  }
  qsort(sums, ent_trace_name_count + 1, sizeof *sums, ent_trace_by_own);
  fprintf(stderr,
          "ent: traced %.3f s on %d threads. Each site: its own time (without "
          "what it called that was timed), all of its time, how often; a "
          "loop's: how much of it the threads worked\n",
          (ended - ent_trace_started) / 1e9, ent_trace_thread_count);
  fprintf(stderr, "%10s %10s %8s %6s  %s\n", "own ms", "all ms", "times",
          "busy", "site");
  for (int64_t k = 0; k <= ent_trace_name_count && k < 40; ++k) {
    ent_trace_sum *sum = &sums[k];
    if (!sum->count)
      continue;
    char busy[16] = "";
    if (sum->kind == ENT_TRACE_LOOP && sum->total > 0 && workers > 0)
      snprintf(busy, sizeof busy, "%5.1f%%",
               100.0 * sum->work / ((double)sum->total * workers));
    fprintf(stderr, "%10.2f %10.2f %8lld %6s  %s\n", sum->own / 1e6,
            sum->total / 1e6, (long long)sum->count, busy,
            ent_trace_name_of(sum->site));
  }
  free(sums);
}

static void ent_trace_finish(void) {
  int64_t ended = ent_trace_now();
  const char *asked = getenv("ENT_TRACE");
  char path[4096];
  if (asked && *asked) {
    // (`%p`: the process's number, for a run of many processes.)
    const char *mark = strstr(asked, "%p");
    if (mark)
      snprintf(path, sizeof path, "%.*s%ld%s", (int)(mark - asked), asked,
               (long)getpid(), mark + 2);
    else
      snprintf(path, sizeof path, "%s", asked);
  } else {
    char program[4096] = "program";
    ssize_t length = readlink("/proc/self/exe", program, sizeof program - 1);
    if (length > 0) {
      program[length] = 0;
      char *slash = strrchr(program, '/');
      memmove(program, slash ? slash + 1 : program,
              strlen(slash ? slash + 1 : program) + 1);
    }
    snprintf(path, sizeof path, "%s.trace.json", program);
  }
  pthread_mutex_lock(&ent_trace_lock);
  ent_trace_say(ended);
  ent_trace_write(path);
  fprintf(stderr, "ent: the trace is in %s (ui.perfetto.dev shows it)\n", path);
  pthread_mutex_unlock(&ent_trace_lock);
}

void ent_trace_start(void) {
  ent_trace_started = ent_trace_now();
  atexit(ent_trace_finish);
}
