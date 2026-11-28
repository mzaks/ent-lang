// The js device (js.ent): JavaScript contexts of QuickJS-ng
// (devices/quickjs, MIT License, see its README), one runtime each, with
// the calls of scripts to the program queued for js_poll.
//
// QuickJS-ng is compiled in here, without its libc part (QJS_BUILD_LIBC
// is not defined): no std, no os module.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#include "quickjs/quickjs-amalgam.c"
#pragma GCC diagnostic pop
#pragma clang diagnostic pop

#include "ent_extern.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

typedef struct {
  int64_t id;
  double due, every; // every: 0 for a timeout
  JSValue fn;
} js_timer;

typedef struct {
  int64_t id;
  JSValue resolve, reject;
} js_waiting;

typedef struct {
  JSValue promise;
  char *why;
} js_rejected;

typedef struct {
  bool used;
  uint32_t uses;
  JSRuntime *rt;
  JSContext *ctx;
  double seconds, deadline;
  char *failed;
  size_t failed_length;
  js_timer *timers;
  int timer_count, timer_room;
  int64_t next_timer, next_call;
  js_waiting *waiting;
  int waiting_count, waiting_room;
  js_rejected *rejected;
  int rejected_count, rejected_room;
  JSValue sources, loaded; // require's: name -> source, name -> exports
  // The memory the runtime holds, counted here (see `held_malloc`), and
  // its limit (0: none); `spare`: allowed past it for now.
  size_t held, limit;
  bool spare;
} js_state;

// ---- Memory.
//
// The limit is kept here, not by QuickJS's own (JS_SetMemoryLimit): when
// an allocation is refused at that limit, QuickJS makes the error that
// says so ("out of memory") with more memory, and where the refused
// allocation left no room under the limit (as small ones do, by where the
// platform's malloc rounds) that error could not be made either: the
// script failed with `null` thrown, and the context had no room even to
// let go of what it held. So the first allocation refused opens a spare
// (an eighth of the limit, at most 1 MB) past the limit, for the error
// and for the calls after to let go of what they hold; it closes when
// what is held is a spare under the limit again. (So the most a context
// holds is its limit and a spare.) Each block carries its size in
// front, so that what is counted is the same on every platform.
enum { held_header = 16 };
static size_t spare_of(const js_state *s) {
  size_t spare = s->limit / 8;
  return spare < (1u << 20) ? spare : (1u << 20);
}
static bool held_fits(js_state *s, size_t more) {
  if (!s->limit || s->held + more <= s->limit)
    return true;
  if (!s->spare) {
    s->spare = true; // (this one refused; what follows may use the spare)
    return false;
  }
  return s->held + more <= s->limit + spare_of(s);
}
static void *held_block(js_state *s, char *block, size_t size) {
  if (!block)
    return NULL;
  memcpy(block, &size, sizeof size);
  s->held += size;
  return block + held_header;
}
static size_t held_size(const void *ptr) {
  size_t size;
  memcpy(&size, (const char *)ptr - held_header, sizeof size);
  return size;
}
static void held_less(js_state *s, size_t size) {
  s->held -= size;
  if (s->spare && s->held + spare_of(s) <= s->limit)
    s->spare = false;
}
static void *held_malloc(void *opaque, size_t size) {
  js_state *s = opaque;
  if (!held_fits(s, size))
    return NULL;
  return held_block(s, malloc(held_header + size), size);
}
static void *held_calloc(void *opaque, size_t count, size_t size) {
  js_state *s = opaque;
  if (size && count > ((size_t)-1 - held_header) / size)
    return NULL;
  if (!held_fits(s, count * size))
    return NULL;
  return held_block(s, calloc(1, held_header + count * size), count * size);
}
static void held_free(void *opaque, void *ptr) {
  if (!ptr)
    return;
  held_less(opaque, held_size(ptr));
  free((char *)ptr - held_header);
}
static void *held_realloc(void *opaque, void *ptr, size_t size) {
  js_state *s = opaque;
  if (!ptr)
    return held_malloc(opaque, size);
  size_t old = held_size(ptr);
  if (size > old && !held_fits(s, size - old))
    return NULL;
  char *block = realloc((char *)ptr - held_header, held_header + size);
  if (!block)
    return NULL;
  memcpy(block, &size, sizeof size);
  s->held += size;
  held_less(s, old);
  return block + held_header;
}
static size_t held_usable(const void *ptr) { return held_size(ptr); }
static const JSMallocFunctions held_functions = {
    held_calloc, held_malloc, held_free, held_realloc, held_usable};

typedef struct {
  int64_t js, call;
  char *kind, *data;
} js_call;

static js_state **states;
static int state_count;
static js_call *queued, *polled;
static int queued_count, queued_room, polled_count;

static void *grown(void *old, size_t size) {
  void *p = realloc(old, size);
  if (!p)
    abort();
  return p;
}
static const ent_text *text_of(const char *bytes, size_t length) {
  if (!bytes || !length)
    return NULL;
  ent_text *text = ent_text_new((uint32_t)length);
  memcpy(text->bytes, bytes, length);
  return text;
}
// A text's bytes, 0-ended (QuickJS wants that of what it parses).
static char *bytes_of(const ent_text *t, size_t *length) {
  size_t n = t ? t->length : 0;
  char *s = grown(NULL, n + 1);
  if (n)
    memcpy(s, t->bytes, n);
  s[n] = 0;
  if (length)
    *length = n;
  return s;
}
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int64_t handle_of(int row) { return ((int64_t)states[row]->uses << 32) | (int64_t)(row + 1); }
static js_state *state_of(int64_t handle) {
  int64_t row = (handle & 0xffffffff) - 1;
  if (row < 0 || row >= state_count || !states[row]->used ||
      states[row]->uses != (uint32_t)(handle >> 32))
    return NULL;
  return states[row];
}
static js_state *state_of_ctx(JSContext *ctx) { return JS_GetContextOpaque(ctx); }

static void say_failed(js_state *s, const char *what, size_t length) {
  if (!length)
    return;
  size_t at = s->failed_length;
  s->failed = grown(s->failed, at + length + 2);
  if (at)
    s->failed[at++] = '\n';
  memcpy(s->failed + at, what, length);
  s->failed_length = at + length;
  s->failed[s->failed_length] = 0;
}
static void forget_failed(js_state *s) {
  s->failed_length = 0;
  if (s->failed)
    s->failed[0] = 0;
}

// What a thrown value says: "Name: message" and its stack, or the words
// of a limit.
static char *words_of(JSContext *ctx, JSValueConst thrown) {
  char *out = NULL;
  if (JS_IsError(thrown)) {
    JSValue name = JS_GetPropertyStr(ctx, thrown, "name");
    JSValue message = JS_GetPropertyStr(ctx, thrown, "message");
    JSValue stack = JS_GetPropertyStr(ctx, thrown, "stack");
    const char *n = JS_ToCString(ctx, name);
    const char *m = JS_ToCString(ctx, message);
    const char *k = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    if (n && m && !strcmp(n, "InternalError") &&
        (!strcmp(m, "interrupted") || !strcmp(m, "out of memory"))) {
      out = strdup(m);
    } else {
      size_t room = (n ? strlen(n) : 5) + (m ? strlen(m) : 0) + (k ? strlen(k) : 0) + 8;
      out = grown(NULL, room);
      snprintf(out, room, "%s: %s%s%s", n ? n : "Error", m ? m : "", k && *k ? "\n" : "",
               k ? k : "");
      size_t l = strlen(out);
      while (l && out[l - 1] == '\n')
        out[--l] = 0;
    }
    JS_FreeCString(ctx, n);
    JS_FreeCString(ctx, m);
    JS_FreeCString(ctx, k);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, stack);
  } else {
    const char *v = JS_ToCString(ctx, thrown);
    const char *prefix = "Uncaught ";
    size_t room = strlen(prefix) + (v ? strlen(v) : 9) + 1;
    out = grown(NULL, room);
    snprintf(out, room, "%s%s", prefix, v ? v : "(nothing)");
    JS_FreeCString(ctx, v);
  }
  return out;
}
// The exception now thrown in `s`, said in its failures.
static void caught(js_state *s) {
  JSValue thrown = JS_GetException(s->ctx);
  char *w = words_of(s->ctx, thrown);
  say_failed(s, w, strlen(w));
  free(w);
  JS_FreeValue(s->ctx, thrown);
  JS_ResetUncatchableError(s->ctx);
}

// Entering the context: the time it has from now on.
static void enter(js_state *s) {
  s->deadline = s->seconds > 0 ? now() + s->seconds : 0;
  JS_UpdateStackTop(s->rt);
}
static int interrupt(JSRuntime *rt, void *opaque) {
  (void)rt;
  js_state *s = opaque;
  return s->deadline > 0 && now() > s->deadline;
}

static void run_jobs(js_state *s) {
  for (;;) {
    JSContext *ctx = NULL;
    enter(s);
    int done = JS_ExecutePendingJob(s->rt, &ctx);
    if (done == 0)
      break;
    if (done < 0)
      caught(s);
  }
}
// Promises rejected that nothing handled once the jobs are done.
static void tell_rejected(js_state *s) {
  for (int i = 0; i < s->rejected_count; ++i) {
    say_failed(s, s->rejected[i].why, strlen(s->rejected[i].why));
    free(s->rejected[i].why);
    JS_FreeValue(s->ctx, s->rejected[i].promise);
  }
  s->rejected_count = 0;
}
static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool handled,
                      void *opaque) {
  (void)opaque;
  js_state *s = state_of_ctx(ctx);
  if (!s)
    return;
  if (handled) {
    for (int i = 0; i < s->rejected_count; ++i)
      if (JS_VALUE_GET_PTR(s->rejected[i].promise) == JS_VALUE_GET_PTR(promise)) {
        free(s->rejected[i].why);
        JS_FreeValue(ctx, s->rejected[i].promise);
        s->rejected[i] = s->rejected[--s->rejected_count];
        return;
      }
    return;
  }
  if (s->rejected_count == s->rejected_room) {
    s->rejected_room = s->rejected_room ? s->rejected_room * 2 : 8;
    s->rejected = grown(s->rejected, (size_t)s->rejected_room * sizeof(js_rejected));
  }
  char *w = words_of(ctx, reason);
  const char *prefix = "not handled: ";
  char *why = grown(NULL, strlen(prefix) + strlen(w) + 1);
  strcpy(why, prefix);
  strcat(why, w);
  free(w);
  s->rejected[s->rejected_count++] = (js_rejected){JS_DupValue(ctx, promise), why};
}

// --- What scripts have ---------------------------------------------------

static JSValue js_host(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  js_state *s = state_of_ctx(ctx);
  const char *kind = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
  if (!kind)
    return JS_ThrowTypeError(ctx, "host(kind, data): kind is no text");
  JSValue json = argc > 1 ? JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED)
                          : JS_UNDEFINED;
  if (JS_IsException(json)) {
    JS_FreeCString(ctx, kind);
    return json;
  }
  const char *data = JS_IsUndefined(json) ? NULL : JS_ToCString(ctx, json);
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise)) {
    JS_FreeCString(ctx, kind);
    JS_FreeCString(ctx, data);
    JS_FreeValue(ctx, json);
    return promise;
  }
  int64_t id = ++s->next_call;
  if (s->waiting_count == s->waiting_room) {
    s->waiting_room = s->waiting_room ? s->waiting_room * 2 : 8;
    s->waiting = grown(s->waiting, (size_t)s->waiting_room * sizeof(js_waiting));
  }
  s->waiting[s->waiting_count++] = (js_waiting){id, funcs[0], funcs[1]};
  if (queued_count == queued_room) {
    queued_room = queued_room ? queued_room * 2 : 64;
    queued = grown(queued, (size_t)queued_room * sizeof(js_call));
  }
  int row = 0;
  while (states[row] != s)
    ++row;
  queued[queued_count++] =
      (js_call){handle_of(row), id, strdup(kind), data ? strdup(data) : NULL};
  JS_FreeCString(ctx, kind);
  JS_FreeCString(ctx, data);
  JS_FreeValue(ctx, json);
  return promise;
}

static JSValue add_timer(JSContext *ctx, int argc, JSValueConst *argv, bool repeat) {
  js_state *s = state_of_ctx(ctx);
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "the timer's callback is no function");
  double ms = 0;
  if (argc > 1 && JS_ToFloat64(ctx, &ms, argv[1]))
    return JS_EXCEPTION;
  if (!(ms >= 0))
    ms = 0;
  if (repeat && ms < 1)
    ms = 1;
  if (s->timer_count == s->timer_room) {
    s->timer_room = s->timer_room ? s->timer_room * 2 : 8;
    s->timers = grown(s->timers, (size_t)s->timer_room * sizeof(js_timer));
  }
  int64_t id = ++s->next_timer;
  s->timers[s->timer_count++] =
      (js_timer){id, now() + ms / 1000.0, repeat ? ms / 1000.0 : 0, JS_DupValue(ctx, argv[0])};
  return JS_NewInt64(ctx, id);
}
static JSValue js_set_timeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  return add_timer(ctx, argc, argv, false);
}
static JSValue js_set_interval(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  return add_timer(ctx, argc, argv, true);
}
static JSValue js_clear_timer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  js_state *s = state_of_ctx(ctx);
  double id = 0;
  if (argc < 1 || JS_ToFloat64(ctx, &id, argv[0]))
    return JS_UNDEFINED;
  for (int i = 0; i < s->timer_count; ++i)
    if ((double)s->timers[i].id == id) {
      JS_FreeValue(ctx, s->timers[i].fn);
      memmove(s->timers + i, s->timers + i + 1, (size_t)(s->timer_count - i - 1) * sizeof(js_timer));
      --s->timer_count;
      break;
    }
  return JS_UNDEFINED;
}
static JSValue js_queue_microtask(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "queueMicrotask: no function");
  // (A promise resolved now runs it as a job.)
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise))
    return promise;
  JSValue then = JS_GetPropertyStr(ctx, promise, "then");
  JSValue r = JS_Call(ctx, then, promise, 1, argv);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, then);
  JSValue c = JS_Call(ctx, funcs[0], JS_UNDEFINED, 0, NULL);
  JS_FreeValue(ctx, c);
  JS_FreeValue(ctx, funcs[0]);
  JS_FreeValue(ctx, funcs[1]);
  JS_FreeValue(ctx, promise);
  return JS_UNDEFINED;
}

static void random_bytes(uint8_t *p, size_t n) {
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__)
  arc4random_buf(p, n);
#else
  while (n) {
    size_t k = n > 256 ? 256 : n;
    if (getentropy(p, k) != 0)
      abort();
    p += k;
    n -= k;
  }
#endif
}
static JSValue js_get_random_values(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "getRandomValues: no array");
  size_t offset = 0, length = 0, per = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &length, &per);
  if (JS_IsException(buffer))
    return buffer;
  size_t size = 0;
  uint8_t *bytes = JS_GetArrayBuffer(ctx, &size, buffer);
  JS_FreeValue(ctx, buffer);
  if (!bytes)
    return JS_EXCEPTION;
  if (length > 65536)
    return JS_ThrowRangeError(ctx, "getRandomValues: more than 65536 bytes");
  random_bytes(bytes + offset, length);
  return JS_DupValue(ctx, argv[0]);
}

static JSValue js_require(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  (void)t;
  js_state *s = state_of_ctx(ctx);
  const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
  if (!name)
    return JS_ThrowTypeError(ctx, "require: no name");
  JSValue have = JS_GetPropertyStr(ctx, s->loaded, name);
  if (!JS_IsUndefined(have)) {
    JS_FreeCString(ctx, name);
    return have;
  }
  JSValue source = JS_GetPropertyStr(ctx, s->sources, name);
  if (!JS_IsString(source)) {
    JS_FreeValue(ctx, source);
    JSValue e = JS_ThrowReferenceError(ctx, "no module '%s'", name);
    JS_FreeCString(ctx, name);
    return e;
  }
  size_t length = 0;
  const char *code = JS_ToCStringLen(ctx, &length, source);
  JS_FreeValue(ctx, source);
  const char *head = "(function (module, exports, require) {";
  const char *tail = "\n})";
  size_t whole = strlen(head) + length + strlen(tail);
  char *wrapped = grown(NULL, whole + 1);
  strcpy(wrapped, head);
  memcpy(wrapped + strlen(head), code, length);
  strcpy(wrapped + strlen(head) + length, tail);
  JS_FreeCString(ctx, code);
  JSValue fn = JS_Eval(ctx, wrapped, whole, name, JS_EVAL_TYPE_GLOBAL);
  free(wrapped);
  if (JS_IsException(fn)) {
    JS_FreeCString(ctx, name);
    return fn;
  }
  JSValue module = JS_NewObject(ctx);
  JSValue exports = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, module, "exports", JS_DupValue(ctx, exports));
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue require = JS_GetPropertyStr(ctx, global, "require");
  JSValue args[3] = {module, exports, require};
  JSValue r = JS_Call(ctx, fn, global, 3, args);
  JS_FreeValue(ctx, require);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, exports);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(r)) {
    JS_FreeValue(ctx, module);
    JS_FreeCString(ctx, name);
    return r;
  }
  JS_FreeValue(ctx, r);
  JSValue result = JS_GetPropertyStr(ctx, module, "exports");
  JS_FreeValue(ctx, module);
  JS_SetPropertyStr(ctx, s->loaded, name, JS_DupValue(ctx, result));
  JS_FreeCString(ctx, name);
  return result;
}

static void define(JSContext *ctx, JSValueConst on, const char *name, JSCFunction *fn, int length) {
  JS_DefinePropertyValueStr(ctx, on, name, JS_NewCFunction(ctx, fn, name, length),
                            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

// --- The procs -----------------------------------------------------------

int64_t ent_js_js_open(int64_t memory, double seconds) {
  int row = 0;
  while (row < state_count && states[row]->used)
    ++row;
  if (row == state_count) {
    states = grown(states, (size_t)(state_count + 1) * sizeof(js_state *));
    states[state_count] = calloc(1, sizeof(js_state));
    if (!states[state_count])
      abort();
    ++state_count;
  }
  js_state *s = states[row];
  uint32_t uses = s->uses + 1;
  memset(s, 0, sizeof *s);
  s->uses = uses;
  s->limit = memory > 0 ? (size_t)memory : 0;
  s->rt = JS_NewRuntime2(&held_functions, s);
  if (!s->rt)
    return 0;
  s->ctx = JS_NewContext(s->rt);
  if (!s->ctx) {
    JS_FreeRuntime(s->rt);
    return 0;
  }
  s->used = true;
  s->seconds = seconds;
#ifdef __EMSCRIPTEN__
  JS_SetMaxStackSize(s->rt, 512 * 1024);
#else
  JS_SetMaxStackSize(s->rt, 4 * 1024 * 1024);
#endif
  JS_SetInterruptHandler(s->rt, interrupt, s);
  JS_SetHostPromiseRejectionTracker(s->rt, rejection, NULL);
  JS_SetContextOpaque(s->ctx, s);
  JSContext *ctx = s->ctx;
  s->sources = JS_NewObject(ctx);
  s->loaded = JS_NewObject(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  define(ctx, global, "host", js_host, 2);
  define(ctx, global, "setTimeout", js_set_timeout, 2);
  define(ctx, global, "setInterval", js_set_interval, 2);
  define(ctx, global, "clearTimeout", js_clear_timer, 1);
  define(ctx, global, "clearInterval", js_clear_timer, 1);
  define(ctx, global, "queueMicrotask", js_queue_microtask, 1);
  define(ctx, global, "require", js_require, 1);
  JSValue crypto = JS_NewObject(ctx);
  define(ctx, crypto, "getRandomValues", js_get_random_values, 1);
  JS_DefinePropertyValueStr(ctx, global, "crypto", crypto,
                            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
  JS_FreeValue(ctx, global);
  return handle_of(row);
}

void ent_js_js_limit(int64_t js, int64_t memory, double seconds) {
  js_state *s = state_of(js);
  if (!s)
    return;
  s->limit = memory > 0 ? (size_t)memory : 0;
  s->spare = false;
  s->seconds = seconds;
}

const ent_text *ent_js_js_eval(int64_t js, const ent_text *code, const ent_text *name) {
  js_state *s = state_of(js);
  if (!s)
    return NULL;
  forget_failed(s);
  size_t length = 0;
  char *source = bytes_of(code, &length);
  char *file = bytes_of(name, NULL);
  enter(s);
  JSValue v = JS_Eval(s->ctx, source, length, *file ? file : "<eval>", JS_EVAL_TYPE_GLOBAL);
  free(source);
  free(file);
  const ent_text *out = NULL;
  if (JS_IsException(v)) {
    caught(s);
  } else {
    if (!JS_IsFunction(s->ctx, v) && !JS_IsPromise(v)) {
      JSValue json = JS_JSONStringify(s->ctx, v, JS_UNDEFINED, JS_UNDEFINED);
      if (JS_IsException(json))
        caught(s);
      else if (!JS_IsUndefined(json)) {
        size_t l = 0;
        const char *c = JS_ToCStringLen(s->ctx, &l, json);
        out = text_of(c, l);
        JS_FreeCString(s->ctx, c);
      }
      JS_FreeValue(s->ctx, json);
    }
    JS_FreeValue(s->ctx, v);
  }
  run_jobs(s);
  tell_rejected(s);
  return out;
}

const ent_text *ent_js_js_failed(int64_t js) {
  js_state *s = state_of(js);
  return s ? text_of(s->failed, s->failed_length) : NULL;
}

bool ent_js_js_set(int64_t js, const ent_text *name, const ent_text *json) {
  js_state *s = state_of(js);
  if (!s)
    return false;
  size_t length = 0;
  char *j = bytes_of(json, &length);
  char *n = bytes_of(name, NULL);
  enter(s);
  JSValue v = JS_ParseJSON(s->ctx, j, length, "<json>");
  bool ok = !JS_IsException(v);
  if (ok) {
    JSValue global = JS_GetGlobalObject(s->ctx);
    ok = JS_SetPropertyStr(s->ctx, global, n, v) >= 0;
    JS_FreeValue(s->ctx, global);
  }
  if (!ok) {
    JSValue e = JS_GetException(s->ctx);
    JS_FreeValue(s->ctx, e);
  }
  free(j);
  free(n);
  return ok;
}

const ent_text *ent_js_js_get(int64_t js, const ent_text *name) {
  js_state *s = state_of(js);
  if (!s)
    return NULL;
  char *n = bytes_of(name, NULL);
  enter(s);
  JSValue global = JS_GetGlobalObject(s->ctx);
  JSValue v = JS_GetPropertyStr(s->ctx, global, n);
  free(n);
  JS_FreeValue(s->ctx, global);
  const ent_text *out = NULL;
  JSValue json = JS_JSONStringify(s->ctx, v, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JSValue e = JS_GetException(s->ctx);
    JS_FreeValue(s->ctx, e);
  } else if (!JS_IsUndefined(json)) {
    size_t l = 0;
    const char *c = JS_ToCStringLen(s->ctx, &l, json);
    out = text_of(c, l);
    JS_FreeCString(s->ctx, c);
  }
  JS_FreeValue(s->ctx, json);
  JS_FreeValue(s->ctx, v);
  return out;
}

void ent_js_js_module(int64_t js, const ent_text *name, const ent_text *source) {
  js_state *s = state_of(js);
  if (!s)
    return;
  char *n = bytes_of(name, NULL);
  JS_SetPropertyStr(s->ctx, s->sources, n,
                    JS_NewStringLen(s->ctx, source ? source->bytes : "", source ? source->length : 0));
  JSAtom atom = JS_NewAtom(s->ctx, n);
  JS_DeleteProperty(s->ctx, s->loaded, atom, 0);
  JS_FreeAtom(s->ctx, atom);
  free(n);
}

// Fires the timers of `s` that are due now; whether any were.
static bool fire_timers(js_state *s) {
  bool any = false;
  double t = now();
  for (;;) {
    int due = -1;
    for (int i = 0; i < s->timer_count; ++i)
      if (s->timers[i].due <= t && (due < 0 || s->timers[i].due < s->timers[due].due))
        due = i;
    if (due < 0)
      break;
    any = true;
    JSValue fn = JS_DupValue(s->ctx, s->timers[due].fn);
    if (s->timers[due].every > 0) {
      s->timers[due].due = t + s->timers[due].every;
    } else {
      JS_FreeValue(s->ctx, s->timers[due].fn);
      memmove(s->timers + due, s->timers + due + 1,
              (size_t)(s->timer_count - due - 1) * sizeof(js_timer));
      --s->timer_count;
    }
    enter(s);
    JSValue r = JS_Call(s->ctx, fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r))
      caught(s);
    JS_FreeValue(s->ctx, r);
    JS_FreeValue(s->ctx, fn);
    run_jobs(s);
    tell_rejected(s);
    // (An interval that is due again at once waits for the next poll.)
    if (s->timer_count && t + 1.0 < now())
      break;
    t = now();
    bool again = false;
    for (int i = 0; i < s->timer_count; ++i)
      if (s->timers[i].due <= t && s->timers[i].every == 0)
        again = true;
    if (!again)
      break;
  }
  return any;
}

int32_t ent_js_js_poll(double wait, int32_t *count, int32_t capacity, int64_t *js, int64_t *call) {
  for (int i = 0; i < polled_count; ++i) {
    free(polled[i].kind);
    free(polled[i].data);
  }
  polled_count = 0;
  double until = now() + wait;
  for (;;) {
    for (int row = 0; row < state_count; ++row)
      if (states[row]->used) {
        fire_timers(states[row]);
        if (JS_IsJobPending(states[row]->rt)) {
          run_jobs(states[row]);
          tell_rejected(states[row]);
        }
      }
    if (queued_count || now() >= until)
      break;
    double next = until;
    for (int row = 0; row < state_count; ++row)
      if (states[row]->used)
        for (int i = 0; i < states[row]->timer_count; ++i)
          if (states[row]->timers[i].due < next)
            next = states[row]->timers[i].due;
    double left = next - now();
    if (left > 0)
      usleep((useconds_t)(left * 1e6) + 1);
  }
  int given = 0;
  polled = grown(polled, (size_t)(capacity > 0 ? capacity : 1) * sizeof(js_call));
  while (given < queued_count && given < capacity) {
    js[given] = queued[given].js;
    call[given] = queued[given].call;
    polled[given] = queued[given];
    ++given;
  }
  polled_count = given;
  memmove(queued, queued + given, (size_t)(queued_count - given) * sizeof(js_call));
  queued_count -= given;
  *count = given;
  return given;
}

const ent_text *ent_js_js_call_kind(int32_t i) {
  if (i < 0 || i >= polled_count)
    return NULL;
  return text_of(polled[i].kind, polled[i].kind ? strlen(polled[i].kind) : 0);
}
const ent_text *ent_js_js_call_data(int32_t i) {
  if (i < 0 || i >= polled_count)
    return NULL;
  return text_of(polled[i].data, polled[i].data ? strlen(polled[i].data) : 0);
}

void ent_js_js_answer(int64_t js, int64_t id, bool ok, const ent_text *json) {
  js_state *s = state_of(js);
  if (!s)
    return;
  int at = -1;
  for (int i = 0; i < s->waiting_count; ++i)
    if (s->waiting[i].id == id)
      at = i;
  if (at < 0)
    return;
  js_waiting w = s->waiting[at];
  s->waiting[at] = s->waiting[--s->waiting_count];
  forget_failed(s);
  JSContext *ctx = s->ctx;
  enter(s);
  size_t length = 0;
  char *j = bytes_of(json, &length);
  JSValue value;
  if (ok) {
    value = length ? JS_ParseJSON(ctx, j, length, "<answer>") : JS_UNDEFINED;
    if (JS_IsException(value)) {
      JSValue e = JS_GetException(ctx);
      JS_FreeValue(ctx, e);
      value = JS_NewStringLen(ctx, j, length);
    }
  } else {
    value = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, value, "message", JS_NewStringLen(ctx, j, length));
  }
  free(j);
  JSValue r = JS_Call(ctx, ok ? w.resolve : w.reject, JS_UNDEFINED, 1, (JSValueConst *)&value);
  if (JS_IsException(r))
    caught(s);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, value);
  JS_FreeValue(ctx, w.resolve);
  JS_FreeValue(ctx, w.reject);
  run_jobs(s);
  tell_rejected(s);
}

int32_t ent_js_js_pending(int64_t js) {
  js_state *s = state_of(js);
  if (!s)
    return 0;
  return s->waiting_count + s->timer_count + (JS_IsJobPending(s->rt) ? 1 : 0);
}

void ent_js_js_close(int64_t js) {
  js_state *s = state_of(js);
  if (!s)
    return;
  JSContext *ctx = s->ctx;
  for (int i = 0; i < s->timer_count; ++i)
    JS_FreeValue(ctx, s->timers[i].fn);
  for (int i = 0; i < s->waiting_count; ++i) {
    JS_FreeValue(ctx, s->waiting[i].resolve);
    JS_FreeValue(ctx, s->waiting[i].reject);
  }
  for (int i = 0; i < s->rejected_count; ++i) {
    JS_FreeValue(ctx, s->rejected[i].promise);
    free(s->rejected[i].why);
  }
  // (Its calls not yet polled are dropped.)
  int kept = 0;
  for (int i = 0; i < queued_count; ++i)
    if (queued[i].js == js) {
      free(queued[i].kind);
      free(queued[i].data);
    } else
      queued[kept++] = queued[i];
  queued_count = kept;
  JS_FreeValue(ctx, s->sources);
  JS_FreeValue(ctx, s->loaded);
  // (Jobs left hold values: run none, drop them with the runtime.)
  JS_FreeContext(ctx);
  JS_FreeRuntime(s->rt);
  free(s->timers);
  free(s->waiting);
  free(s->rejected);
  free(s->failed);
  uint32_t uses = s->uses;
  memset(s, 0, sizeof *s);
  s->uses = uses;
}
