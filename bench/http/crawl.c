// The crawler of examples/http in C, on libcurl's multi interface: what
// crawl_live.ent should match (http-SPEC.md, §9.1). The same rules, so
// the same report: breadth first by depth (a page's links are taken once
// every ask less deep is settled), one ask per URL (a hash set of them),
// a redirect as deep as the ask redirected and followed by the crawler
// (curl follows none), tries again for 5xx, 429 and no answer with the
// wait doubling, at most `per_host` asks out to a host, each host's
// robots.txt first (its rules, its Crawl-delay), the links of HTML pages
// found and URLs made as examples/http/urls.ent does (its fns, one for
// one), robots.txt read as robots.ent does.
//
//   cc -O2 -o crawl bench/http/crawl.c -lcurl
//   crawl            (where crawl.conf and live.conf are, as for crawl_live)
//
// The report on stdout, its lines as crawl.ent's `report` says them.
#define _GNU_SOURCE
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------- texts

typedef struct {
  char *p;
  size_t n, room;
} Text;

static void put_bytes(Text *t, const char *s, size_t n) {
  if (t->n + n + 1 > t->room) {
    while (t->n + n + 1 > t->room)
      t->room = t->room ? t->room * 2 : 64;
    t->p = realloc(t->p, t->room);
  }
  memcpy(t->p + t->n, s, n);
  t->n += n;
  t->p[t->n] = 0;
}
static void put_text(Text *t, const char *s) { put_bytes(t, s, strlen(s)); }

static char *copy(const char *s, size_t n) {
  char *c = malloc(n + 1);
  memcpy(c, s, n);
  c[n] = 0;
  return c;
}

// `s` from `from` on: where `what` is first, or -1.
static long find(const char *s, size_t n, const char *what, size_t from) {
  size_t k = strlen(what);
  if (from > n)
    return -1;
  const char *at = memmem(s + from, n - from, what, k);
  return at ? at - s : -1;
}

static char *lowered(const char *s, size_t n) {
  char *c = malloc(n + 1);
  for (size_t i = 0; i < n; i++)
    c[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
  c[n] = 0;
  return c;
}

static long earliest(long a, long b) {
  return a < 0 ? b : b < 0 ? a : a < b ? a : b;
}
static long or_end(long at, long end) { return at < 0 ? end : at; }

// ---------------------------------------------------------------- URLs
// (examples/http/urls.ent)

static long scheme_end(const char *t) {
  for (long i = 0; t[i]; i++) {
    char c = t[i];
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    int other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (c == ':' && i > 0)
      return i;
    if (!(letter || (i > 0 && other)))
      return -1;
  }
  return -1;
}

static int ends_with(const char *s, size_t n, const char *suffix) {
  size_t k = strlen(suffix);
  return n >= k && !memcmp(s + n - k, suffix, k);
}

// A path without dot segments (urls.ent `dots`).
static void without_dots(const char *path, size_t n, Text *out) {
  if (n == 0) {
    put_text(out, "/");
    return;
  }
  size_t start = out->n;
  size_t at = 1;
  for (;;) {
    long next = find(path, n, "/", at);
    size_t end = next < 0 ? n : (size_t)next;
    const char *segment = path + at;
    size_t length = end - at;
    int up = length == 2 && segment[0] == '.' && segment[1] == '.';
    int here = length == 1 && segment[0] == '.';
    if (up) {
      size_t cut = start;
      for (size_t i = start; i < out->n; i++)
        if (out->p[i] == '/')
          cut = i;
      out->n = cut;
      out->p[cut] = 0;
    } else if (!here) {
      put_text(out, "/");
      put_bytes(out, segment, length);
    }
    if (next >= 0) {
      at = next + 1;
      continue;
    }
    if (up || here)
      put_text(out, "/");
    return;
  }
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// The percent encodings of unreserved bytes (RFC 3986, 2.3) decoded, in
// place.
static void unreserved_decoded(char *t) {
  char *out = t;
  for (const char *at = t; *at;) {
    if (at[0] == '%' && at[1] && at[2] && hex_value(at[1]) >= 0 &&
        hex_value(at[2]) >= 0) {
      int c = hex_value(at[1]) * 16 + hex_value(at[2]);
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
          c == '~') {
        *out++ = (char)c;
        at += 3;
        continue;
      }
    }
    *out++ = *at++;
  }
  *out = 0;
}

static char *normalise(const char *url) {
  long colon = scheme_end(url);
  size_t n = strlen(url);
  Text out = {0};
  char *scheme = lowered(url, colon);
  const char *rest = url + colon + 3;
  size_t rest_n = n - (colon + 3);
  long host_end = or_end(earliest(earliest(find(rest, rest_n, "/", 0),
                                           find(rest, rest_n, "?", 0)),
                                  find(rest, rest_n, "#", 0)),
                         rest_n);
  char *named = lowered(rest, host_end);
  size_t named_n = host_end;
  if (!strcmp(scheme, "http") && ends_with(named, named_n, ":80"))
    named_n -= 3;
  else if (!strcmp(scheme, "https") && ends_with(named, named_n, ":443"))
    named_n -= 4;
  const char *tail = rest + host_end;
  size_t tail_n = or_end(find(tail, rest_n - host_end, "#", 0), rest_n - host_end);
  long q = find(tail, tail_n, "?", 0);
  size_t path_n = or_end(q, tail_n);
  put_text(&out, scheme);
  put_text(&out, "://");
  put_bytes(&out, named, named_n);
  without_dots(tail, path_n, &out);
  put_bytes(&out, tail + path_n, tail_n - path_n);
  size_t kept = strlen(scheme) + 3 + named_n;
  if (memchr(tail, '%', tail_n))
    unreserved_decoded(out.p + kept);
  // The bytes a URL may not hold percent-encoded (examples/http/urls.ent).
  int unfit = 0;
  for (const unsigned char *c = (unsigned char *)out.p + kept; *c; c++)
    if (*c <= ' ' || *c >= 127 || *c == '"' || *c == '<' || *c == '>')
      unfit = 1;
  if (unfit) {
    Text encoded = {0};
    put_bytes(&encoded, out.p, kept);
    for (const unsigned char *c = (unsigned char *)out.p + kept; *c; c++) {
      if (*c <= ' ' || *c >= 127 || *c == '"' || *c == '<' || *c == '>') {
        char e[4];
        snprintf(e, sizeof e, "%%%02X", *c);
        put_text(&encoded, e);
      } else {
        put_bytes(&encoded, (const char *)c, 1);
      }
    }
    free(out.p);
    out = encoded;
  }
  free(scheme);
  free(named);
  return out.p;
}

// Scheme and host: all before the path.
static size_t origin_length(const char *url) {
  size_t n = strlen(url), from = scheme_end(url) + 3;
  return or_end(earliest(find(url, n, "/", from), find(url, n, "?", from)), n);
}

static char *host_of(const char *url) {
  size_t from = scheme_end(url) + 3;
  return copy(url + from, origin_length(url) - from);
}

// `t` up to `what` (all of it where it has none).
static size_t before(const char *t, size_t n, const char *what) {
  return or_end(find(t, n, what, 0), n);
}

// Where a link goes from the page at `base`; NULL where it is not to be
// followed.
// What the character reference `name` (n bytes, between '&' and ';')
// stands for, written to `out`; 0 where it is not one of those known
// (examples/http/urls.ent: amp, lt, gt, quot, apos, nbsp, numbers of
// printable ASCII).
static int entity(const char *name, size_t n, Text *out) {
  static const struct { const char *name, *by; } known[] = {
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
      {"apos", "'"}, {"nbsp", "\xc2\xa0"}};
  for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
    if (strlen(known[k].name) == n && !memcmp(known[k].name, name, n)) {
      put_text(out, known[k].by);
      return 1;
    }
  if (n < 2 || name[0] != '#')
    return 0;
  int x = name[1] == 'x' || name[1] == 'X';
  size_t from = x ? 2 : 1;
  if (n <= from)
    return 0;
  int v = 0;
  for (size_t i = from; i < n; i++) {
    int d = hex_value(name[i]);
    if (d < 0 || (!x && d > 9))
      return 0;
    v = v * (x ? 16 : 10) + d;
    if (v > 1000)
      return 0;
  }
  if (v < 32 || v >= 127)
    return 0;
  char c = (char)v;
  put_bytes(out, &c, 1);
  return 1;
}

static int blank_byte(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

static char *resolve(const char *base, const char *link, size_t link_n) {
  // References decoded; then the spaces at its ends dropped, and the
  // tabs and line ends in it (as urls.ent's resolve).
  Text d = {0};
  for (size_t at = 0; at < link_n;) {
    char c = link[at];
    if (c == '&') {
      long end = find(link, link_n, ";", at);
      if (end >= 0 && end - (long)at <= 10 &&
          entity(link + at + 1, end - at - 1, &d)) {
        at = end + 1;
        continue;
      }
    }
    put_bytes(&d, &c, 1);
    at++;
  }
  size_t from = 0, to = d.n;
  while (from < to && blank_byte(d.p[from]))
    from++;
  while (to > from && blank_byte(d.p[to - 1]))
    to--;
  Text r = {0};
  for (size_t i = from; i < to; i++)
    if (d.p[i] != '\n' && d.p[i] != '\t' && d.p[i] != '\r')
      put_bytes(&r, d.p + i, 1);
  free(d.p);
  if (!r.p)
    put_text(&r, "");
  char *result = NULL;
  long colon = scheme_end(r.p);
  Text joined = {0};
  if (r.n == 0 || r.p[0] == '#') {
  } else if (colon >= 0) {
    char *scheme = lowered(r.p, colon);
    if (!strcmp(scheme, "http") || !strcmp(scheme, "https"))
      result = normalise(r.p);
    free(scheme);
  } else if (r.n >= 2 && r.p[0] == '/' && r.p[1] == '/') {
    put_bytes(&joined, base, scheme_end(base) + 1);
    put_text(&joined, r.p);
    result = normalise(joined.p);
  } else if (r.p[0] == '/') {
    put_bytes(&joined, base, origin_length(base));
    put_text(&joined, r.p);
    result = normalise(joined.p);
  } else {
    size_t n = strlen(base);
    size_t page = before(base, before(base, n, "#"), "?");
    if (r.p[0] == '?') {
      put_bytes(&joined, base, page);
    } else {
      size_t slash = 0;
      for (size_t i = 0; i < page; i++)
        if (base[i] == '/')
          slash = i + 1;
      put_bytes(&joined, base, slash);
    }
    put_text(&joined, r.p);
    result = normalise(joined.p);
  }
  free(r.p);
  free(joined.p);
  return result;
}

// ---------------------------------------------------------------- links

static void uncommented(const char *page, size_t n, Text *out) {
  size_t from = 0;
  for (;;) {
    long open = find(page, n, "<!--", from);
    if (open < 0) {
      put_bytes(out, page + from, n - from);
      return;
    }
    long close = find(page, n, "-->", open + 4);
    put_bytes(out, page + from, open - from);
    if (close < 0)
      return;
    from = close + 3;
  }
}

// What a page's scripts hold dropped, `<script ..>` kept.
static void unscripted(const char *page, const char *low, size_t n,
                       Text *out) {
  size_t from = 0;
  for (;;) {
    long open = find(low, n, "<script", from);
    long start = open < 0 ? -1 : find(low, n, ">", open);
    if (start < 0) {
      put_bytes(out, page + from, n - from);
      return;
    }
    put_bytes(out, page + from, start + 1 - from);
    long close = find(low, n, "</script", start);
    if (close < 0)
      return;
    from = close;
  }
}

static int space(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

// Whether byte `at` is in a tag and not in a quoted value.
static int in_tag(const char *low, long at) {
  long start = at - 1;
  while (start >= 0 && low[start] != '<' && low[start] != '>')
    start--;
  if (start < 0 || low[start] == '>')
    return 0;
  char quote = 0;
  for (long i = start; i < at; i++) {
    if (!quote) {
      if (low[i] == '"' || low[i] == '\'')
        quote = low[i];
    } else if (low[i] == quote) {
      quote = 0;
    }
  }
  return !quote;
}

// The next `href` or `src` attribute's value (examples/http/urls.ent):
// a name after a space, then '=' with spaces around it allowed, in a tag
// and not in another value.
static long next_value(const char *low, size_t n, size_t from) {
  for (;;) {
    long h = find(low, n, "href", from), s = find(low, n, "src", from);
    long at = earliest(h, s);
    if (at < 0)
      return -1;
    from = at + 1;
    if (!(at > 0 && space(low[at - 1])))
      continue;
    size_t i = at + (at == h ? 4 : 3);
    while (i < n && space(low[i]))
      i++;
    if (i >= n || low[i] != '=')
      continue;
    i++;
    while (i < n && space(low[i]))
      i++;
    if (i >= n || !in_tag(low, at))
      continue;
    return i;
  }
}

static void value_span(const char *page, size_t n, long at, long *from,
                       long *to) {
  char q = (size_t)at < n ? page[at] : 0;
  if (q == '"' || q == '\'') {
    char quote[2] = {q, 0};
    *from = at + 1;
    *to = or_end(find(page, n, quote, at + 1), n);
  } else {
    *from = at;
    long end = n;
    for (size_t i = at; i < n; i++)
      if (space(page[i]) || page[i] == '>') {
        end = i;
        break;
      }
    *to = end;
  }
}

// ---------------------------------------------------------------- robots
// (examples/http/robots.ent)

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

typedef struct {
  int allow;
  char *pattern;
  size_t n;
} Rule;

typedef struct {
  char *key, *value; // NULL key: none
} Line;

static Line line_of(const char *body, size_t at, size_t end) {
  Line l = {0};
  size_t n = before(body + at, end - at, "#");
  const char *line = body + at;
  long colon = find(line, n, ":", 0);
  if (colon <= 0)
    return l;
  size_t a = 0, b = colon;
  while (a < b && blank(line[a]))
    a++;
  while (b > a && blank(line[b - 1]))
    b--;
  l.key = lowered(line + a, b - a);
  a = colon + 1;
  b = n;
  while (a < b && blank(line[a]))
    a++;
  while (b > a && blank(line[b - 1]))
    b--;
  l.value = copy(line + a, b - a);
  return l;
}

static double seconds(const char *t) {
  size_t n = strlen(t), dot = or_end(find(t, n, ".", 0), n);
  char *whole = copy(t, dot);
  double value = (double)strtoll(whole, NULL, 10), unit = 0.1;
  free(whole);
  for (size_t i = dot + 1; i < n; i++)
    if (t[i] >= '0' && t[i] <= '9') {
      value += (t[i] - '0') * unit;
      unit /= 10.0;
    }
  return value;
}

static void robots_rules(const char *body, size_t n, const char *agent,
                         Rule **rules, int *count, double *delay) {
  int named = 0;
  for (size_t at = 0; at < n;) {
    size_t end = or_end(find(body, n, "\n", at), n);
    Line l = line_of(body, at, end);
    if (l.key && !strcmp(l.key, "user-agent")) {
      char *who = lowered(l.value, strlen(l.value));
      named |= !strcmp(who, agent);
      free(who);
    }
    free(l.key);
    free(l.value);
    at = end + 1;
  }
  int agents = 0, ours = 0, room = 0;
  *count = 0;
  *rules = NULL;
  *delay = 0;
  for (size_t at = 0; at < n;) {
    size_t end = or_end(find(body, n, "\n", at), n);
    Line l = line_of(body, at, end);
    if (l.key && !strcmp(l.key, "user-agent")) {
      char *who = lowered(l.value, strlen(l.value));
      int kept = named ? !strcmp(who, agent) : !strcmp(l.value, "*");
      free(who);
      ours = (agents && ours) || kept;
      agents = 1;
    } else if (l.key) {
      agents = 0;
      int allow = !strcmp(l.key, "allow");
      if (ours && (allow || (!strcmp(l.key, "disallow") && l.value[0]))) {
        if (*count == room)
          *rules = realloc(*rules, (room = room ? room * 2 : 8) * sizeof(Rule));
        (*rules)[(*count)++] = (Rule){allow, strdup(l.value), strlen(l.value)};
      }
      if (ours && !strcmp(l.key, "crawl-delay"))
        *delay = seconds(l.value);
    }
    free(l.key);
    free(l.value);
    at = end + 1;
  }
}

static int matches(const char *p, size_t pn, size_t i, const char *s,
                   size_t sn, size_t j) {
  if (i == pn)
    return 1;
  if (p[i] == '$' && i + 1 == pn)
    return j == sn;
  if (p[i] == '*')
    return matches(p, pn, i + 1, s, sn, j) ||
           (j < sn && matches(p, pn, i, s, sn, j + 1));
  return j < sn && p[i] == s[j] && matches(p, pn, i + 1, s, sn, j + 1);
}

static int robots_allow(const Rule *rules, int count, const char *path) {
  long best = -1;
  int allow = 1;
  size_t n = strlen(path);
  for (int r = 0; r < count; r++)
    if (matches(rules[r].pattern, rules[r].n, 0, path, n, 0)) {
      long k = rules[r].n;
      if (k > best || (k == best && rules[r].allow)) {
        best = k;
        allow = rules[r].allow;
      }
    }
  return allow;
}

// ---------------------------------------------------------------- crawl

enum { NEW, WAITING, SENT, DEFERRED, DONE };
enum { NONE_FAILED, TIMED_OUT, FAILED };

typedef struct {
  char *url;
  int host, depth, hops, tries, state;
  int robots, disallowed, slow, redirect_limit;
  double next_at, sent;
  int status, failure; // the last answer
  char *location;
  Text body, headers;
  int *links, link_count, link_room;
  CURL *easy;
} Ask;

typedef struct {
  char *name;
  int busy, known, robots_ask, rule_count;
  Rule *rules;
  double delay, next;
  int *waiting, waiting_first, waiting_count, waiting_room; // a ring
  int *held, held_count, held_room; // till robots.txt is known
} Host;

static Ask *asks;
static int ask_count, ask_room;
static Host *hosts;
static int host_count;
static int32_t *slot_ask;
static size_t slots;
static int max_depth, per_host, retries, redirects;
static double timeout_s, slow_s, backoff_s;
static int level, *open_at, depth_room, open_below, left;
static int *deferred, deferred_count, deferred_room;
// A page put off till its level whose depth was lowered to the level or
// less since (a redirect at a lesser depth to it): its links are due.
static int deferred_due;
static CURLM *multi;
static char *proxy;
static int connections;
static struct timespec epoch;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - epoch.tv_sec) + (t.tv_nsec - epoch.tv_nsec) * 1e-9;
}

static uint64_t fnv(const char *s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 0x100000001b3ull;
  return h;
}

static void push(int **list, int *count, int *room, int value) {
  if (*count == *room)
    *list = realloc(*list, (*room = *room ? *room * 2 : 16) * sizeof(int));
  (*list)[(*count)++] = value;
}

static void ring_push(Host *h, int a) {
  if (h->waiting_count == h->waiting_room) {
    int room = h->waiting_room ? h->waiting_room * 2 : 64;
    int *made = malloc(room * sizeof(int));
    for (int i = 0; i < h->waiting_count; i++)
      made[i] = h->waiting[(h->waiting_first + i) % h->waiting_room];
    free(h->waiting);
    h->waiting = made;
    h->waiting_first = 0;
    h->waiting_room = room;
  }
  h->waiting[(h->waiting_first + h->waiting_count++) % h->waiting_room] = a;
}

// Counts of asks not settled by depth: those at the level or less.
static void count_open(int depth, int step) {
  if (depth >= depth_room) {
    int room = depth_room ? depth_room : 64;
    while (room <= depth)
      room *= 2;
    open_at = realloc(open_at, room * sizeof(int));
    memset(open_at + depth_room, 0, (room - depth_room) * sizeof(int));
    depth_room = room;
  }
  open_at[depth] += step;
  if (depth <= level)
    open_below += step;
}

static void settle(Ask *a) {
  a->state = DONE;
  count_open(a->depth, -1);
  left--;
}

static void lower_depth(Ask *a, int depth) {
  if (depth >= a->depth)
    return;
  if (a->state != DONE) {
    count_open(a->depth, -1);
    count_open(depth, 1);
  }
  a->depth = depth;
  if (a->state == DEFERRED && depth <= level)
    deferred_due = 1;
}

static int host_named(const char *name) {
  for (int h = 0; h < host_count; h++)
    if (!strcmp(hosts[h].name, name))
      return h;
  return -1;
}

static char *path_of(const char *url) {
  size_t o = origin_length(url);
  return url[o] ? strdup(url + o) : strdup("/");
}

// An ask whose host's robots.txt is known: to be sent, or disallowed.
static void place(int id) {
  Ask *a = &asks[id];
  Host *h = &hosts[a->host];
  if (!a->robots) {
    char *path = path_of(a->url);
    int allowed = robots_allow(h->rules, h->rule_count, path);
    free(path);
    if (!allowed) {
      a->disallowed = 1;
      settle(a);
      return;
    }
  }
  a->state = WAITING;
  ring_push(h, id);
}

// The ask for `url` (normalised): the one there is, its depth lowered, or
// a new one; -1 where its host is not to be gone to.
static int ask_for(char *url, int depth, int hops, int robots) {
  uint64_t hash = fnv(url);
  size_t s = hash & (slots - 1);
  for (; slot_ask[s] >= 0; s = (s + 1) & (slots - 1))
    if (!strcmp(asks[slot_ask[s]].url, url)) {
      free(url);
      lower_depth(&asks[slot_ask[s]], depth);
      return slot_ask[s];
    }
  char *name = host_of(url);
  int host = host_named(name);
  free(name);
  if (host < 0) {
    free(url);
    return -1;
  }
  if (ask_count == ask_room) {
    ask_room = ask_room ? ask_room * 2 : 1024;
    asks = realloc(asks, ask_room * sizeof(Ask));
  }
  if ((size_t)(ask_count + 1) * 2 > slots) {
    size_t more = slots * 2;
    int32_t *made = malloc(more * sizeof(int32_t));
    memset(made, 0xff, more * sizeof(int32_t));
    for (int i = 0; i < ask_count; i++) {
      size_t t = fnv(asks[i].url) & (more - 1);
      while (made[t] >= 0)
        t = (t + 1) & (more - 1);
      made[t] = i;
    }
    free(slot_ask);
    slot_ask = made;
    slots = more;
    for (s = hash & (slots - 1); slot_ask[s] >= 0; s = (s + 1) & (slots - 1))
      ;
  }
  int id = ask_count++;
  slot_ask[s] = id;
  Ask *a = &asks[id];
  memset(a, 0, sizeof *a);
  a->url = url;
  a->host = host;
  a->depth = depth;
  a->hops = hops;
  a->robots = robots;
  a->state = NEW;
  count_open(depth, 1);
  left++;
  if (robots || hosts[host].known)
    place(id);
  else
    push(&hosts[host].held, &hosts[host].held_count, &hosts[host].held_room, id);
  return id;
}

// (The ask by its number: `asks` moves as it grows.)
static size_t on_body(char *data, size_t size, size_t n, void *ask) {
  Text *body = &asks[(intptr_t)ask].body;
  if (body->n + size * n > 16777216)
    return 0;
  put_bytes(body, data, size * n);
  return size * n;
}

static size_t on_header(char *data, size_t size, size_t n, void *ask) {
  Text *headers = &asks[(intptr_t)ask].headers;
  if (n * size >= 5 && !memcmp(data, "HTTP/", 5))
    headers->n = 0;
  put_bytes(headers, data, size * n);
  return size * n;
}

static void start(int id) {
  Ask *a = &asks[id];
  CURL *easy = curl_easy_init();
  a->easy = easy;
  a->body.n = a->headers.n = 0;
  curl_easy_setopt(easy, CURLOPT_URL, a->url);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, (void *)(intptr_t)id);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void *)(intptr_t)id);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, (void *)(intptr_t)id);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "crawl.c/0.1");
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)(timeout_s * 1000.0 + 0.5));
  if (proxy && *proxy)
    curl_easy_setopt(easy, CURLOPT_PROXY, proxy);
  curl_multi_add_handle(multi, easy);
  a->state = SENT;
  a->sent = now();
  hosts[a->host].busy++;
}

static int is_html(const Text *headers) {
  char *low = lowered(headers->p ? headers->p : "", headers->n);
  int html = strstr(low, "content-type: text/html") != NULL;
  free(low);
  return html;
}

static void extract_links(int id) {
  Ask *a = &asks[id];
  Text page = {0};
  uncommented(a->body.p ? a->body.p : "", a->body.n, &page);
  size_t n = page.n;
  const char *text = page.p ? page.p : "";
  char *low = lowered(text, n);
  if (find(low, n, "<script", 0) >= 0) {
    Text plain = {0};
    unscripted(text, low, n, &plain);
    free(page.p);
    free(low);
    page = plain;
    n = page.n;
    text = page.p ? page.p : "";
    low = lowered(text, n);
  }
  // Its base: `<base href>`, else its own URL.
  char *base = NULL;
  long skip = -1, tag = find(low, n, "<base", 0);
  long at = tag < 0 ? -1 : next_value(low, n, tag);
  if (at >= 0) {
    long from, to;
    value_span(text, n, at, &from, &to);
    base = resolve(a->url, text + from, to - from);
    skip = at;
  }
  if (!base)
    base = strdup(a->url);
  int depth = a->depth;
  for (size_t from = 0;;) {
    at = next_value(low, n, from);
    if (at < 0)
      break;
    from = at + 1;
    if (at == skip)
      continue;
    long start_at, end_at;
    value_span(text, n, at, &start_at, &end_at);
    char *target = resolve(base, text + start_at, end_at - start_at);
    if (!target)
      continue;
    char *name = host_of(target);
    int allowed = name[0] && host_named(name) >= 0;
    free(name);
    if (!allowed) {
      free(target);
      continue;
    }
    int to = ask_for(target, depth + 1, 0, 0);
    a = &asks[id]; // (asks may have moved)
    push(&a->links, &a->link_count, &a->link_room, to);
  }
  free(base);
  free(low);
  free(page.p);
}

static int retryable(int status) { return status >= 500 || status == 429; }

static void try_again(Ask *a, double t) {
  a->tries++;
  double wait = backoff_s;
  for (int i = 1; i < a->tries; i++)
    wait *= 2.0;
  a->next_at = t + wait;
  a->state = WAITING;
  ring_push(&hosts[a->host], a - asks);
}

static void robots_known(Host *h, Ask *a) {
  if (a->failure == NONE_FAILED && a->status == 200)
    robots_rules(a->body.p ? a->body.p : "", a->body.n, "ent-crawl", &h->rules,
                 &h->rule_count, &h->delay);
  h->known = 1;
  for (int i = 0; i < h->held_count; i++)
    place(h->held[i]);
  h->held_count = 0;
}

static void answered(CURL *easy, CURLcode result) {
  intptr_t id;
  curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&id);
  Ask *a = &asks[id];
  double t = now();
  long status = 0;
  char *location = NULL;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &location);
  hosts[a->host].busy--;
  a->status = (int)status;
  a->failure = result == CURLE_OK ? NONE_FAILED
               : result == CURLE_OPERATION_TIMEDOUT ? TIMED_OUT
                                                    : FAILED;
  free(a->location);
  a->location = location ? strdup(location) : NULL;
  curl_multi_remove_handle(multi, easy);
  curl_easy_cleanup(easy);
  a->easy = NULL;
  if (t - a->sent >= slow_s)
    a->slow = 1;
  if (a->failure != NONE_FAILED || retryable(a->status)) {
    if (a->tries < retries) {
      try_again(a, t);
      return;
    }
  }
  if (a->robots)
    robots_known(&hosts[a->host], a);
  a = &asks[id];
  if (a->failure == NONE_FAILED && a->status >= 300 && a->status < 400 &&
      a->location && a->location[0]) {
    char *target = resolve(a->url, a->location, strlen(a->location));
    if (target) {
      if (a->hops >= redirects) {
        a->redirect_limit = 1;
        free(target);
      } else {
        ask_for(target, a->depth, a->hops + 1, 0);
        a = &asks[id];
      }
    }
  }
  if (a->failure == NONE_FAILED && a->status == 200 && a->depth < max_depth &&
      is_html(&a->headers)) {
    if (a->depth <= level) {
      extract_links(id);
      a = &asks[id];
    } else {
      a->state = DEFERRED;
      push(&deferred, &deferred_count, &deferred_room, id);
      return;
    }
  }
  free(a->body.p);
  a->body = (Text){0};
  settle(a);
}

// The asks of each host that may go now: at most `per_host` out, and
// its Crawl-delay apart; the others wait their turn. Says when the next
// one that waits is due (or a large number).
static double send_due(double t) {
  double next_due = 1e300;
  for (int h = 0; h < host_count; h++) {
    Host *host = &hosts[h];
    int count = host->waiting_count;
    for (int i = 0; i < count && host->busy < per_host; i++) {
      int id = host->waiting[host->waiting_first];
      host->waiting_first = (host->waiting_first + 1) % host->waiting_room;
      host->waiting_count--;
      Ask *a = &asks[id];
      if (a->next_at > t) {
        if (a->next_at < next_due)
          next_due = a->next_at;
        ring_push(host, id);
        continue;
      }
      if (host->delay > 0 && host->next > t) {
        if (host->next < next_due)
          next_due = host->next;
        ring_push(host, id);
        continue;
      }
      if (host->delay > 0)
        host->next = t + host->delay;
      start(id);
    }
  }
  return next_due;
}

static char *setting(const char *conf, const char *key) {
  size_t k = strlen(key);
  for (const char *at = conf; at && *at;) {
    const char *end = strchr(at, '\n');
    size_t n = end ? (size_t)(end - at) : strlen(at);
    if (n > k && !strncmp(at, key, k) && at[k] == ' ')
      return copy(at + k + 1, n - k - 1);
    at = end ? end + 1 : NULL;
  }
  return strdup("");
}

static char *read_file(const char *name) {
  FILE *f = fopen(name, "rb");
  if (!f)
    return strdup("");
  Text t = {0};
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
    put_bytes(&t, buffer, n);
  fclose(f);
  return t.p ? t.p : strdup("");
}

static void report(void) {
  Text out = {0};
  char line[256];
  for (int id = 0; id < ask_count; id++) {
    Ask *a = &asks[id];
    char word[32];
    if (a->failure == TIMED_OUT)
      strcpy(word, "timeout");
    else if (a->failure == FAILED)
      strcpy(word, "failed");
    else
      snprintf(word, sizeof word, "%d", a->status);
    if (a->disallowed) {
      put_text(&out, "disallowed ");
      put_text(&out, a->url);
      put_text(&out, "\n");
    } else {
      snprintf(line, sizeof line, " %s %d\n", word, a->tries + 1);
      put_text(&out, "fetched ");
      put_text(&out, a->url);
      put_text(&out, line);
    }
    if (!a->disallowed && a->failure == NONE_FAILED && a->status >= 300 &&
        a->status < 400 && a->location && a->location[0]) {
      char *target = resolve(a->url, a->location, strlen(a->location));
      put_text(&out, "redirect ");
      put_text(&out, a->url);
      put_text(&out, " ");
      put_text(&out, target ? target : "");
      put_text(&out, "\n");
      free(target);
    }
    if (a->redirect_limit) {
      put_text(&out, "redirect-limit ");
      put_text(&out, a->url);
      put_text(&out, "\n");
    }
    if (a->slow) {
      put_text(&out, "slow ");
      put_text(&out, a->url);
      put_text(&out, "\n");
    }
    for (int k = 0; k < a->link_count; k++) {
      Ask *to = &asks[a->links[k]];
      if (to->disallowed)
        continue;
      int ok = to->failure == NONE_FAILED && to->status >= 200 && to->status < 400;
      if (ok)
        continue;
      char to_word[32];
      if (to->failure == TIMED_OUT)
        strcpy(to_word, "timeout");
      else if (to->failure == FAILED)
        strcpy(to_word, "failed");
      else
        snprintf(to_word, sizeof to_word, "%d", to->status);
      put_text(&out, "broken ");
      put_text(&out, to->url);
      put_text(&out, " ");
      put_text(&out, to_word);
      put_text(&out, " from ");
      put_text(&out, a->url);
      put_text(&out, "\n");
    }
  }
  fwrite(out.p, 1, out.n, stdout);
  free(out.p);
}

int main(void) {
  clock_gettime(CLOCK_MONOTONIC, &epoch);
  char *conf = read_file("crawl.conf"), *live = read_file("live.conf");
  char *seed = setting(conf, "seed"), *allow = setting(conf, "allow"),
       *text;
  max_depth = atoi(text = setting(conf, "max_depth"));
  free(text);
  per_host = atoi(text = setting(conf, "per_host"));
  free(text);
  retries = atoi(text = setting(conf, "retries"));
  free(text);
  redirects = atoi(text = setting(conf, "redirects"));
  free(text);
  timeout_s = atof(text = setting(conf, "timeout_ms")) / 1000.0;
  free(text);
  slow_s = atof(text = setting(conf, "slow_ms")) / 1000.0;
  free(text);
  backoff_s = atof(text = setting(conf, "backoff_ms")) / 1000.0;
  free(text);
  proxy = setting(live, "proxy");
  connections = atoi(text = setting(live, "connections"));
  free(text);
  if (connections <= 0)
    connections = 64;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  multi = curl_multi_init();
  curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)connections);
  curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)connections);
  slots = 1024;
  slot_ask = malloc(slots * sizeof(int32_t));
  memset(slot_ask, 0xff, slots * sizeof(int32_t));

  char *start_url = normalise(seed);
  size_t scheme = scheme_end(start_url);
  // Every host allowed, with the ask for its robots.txt first.
  hosts = calloc(strlen(allow) + 1, sizeof(Host));
  for (char *name = strtok(allow, " "); name; name = strtok(NULL, " ")) {
    Host *h = &hosts[host_count++];
    h->name = strdup(name);
  }
  for (int h = 0; h < host_count; h++) {
    Text url = {0};
    put_bytes(&url, start_url, scheme);
    put_text(&url, "://");
    put_text(&url, hosts[h].name);
    put_text(&url, "/robots.txt");
    hosts[h].robots_ask = ask_for(url.p, 0, 0, 1);
  }
  ask_for(start_url, 0, 0, 0);

  while (left > 0) {
    double t = now();
    double next_due = send_due(t);
    int running = 0;
    curl_multi_perform(multi, &running);
    CURLMsg *message;
    int queued;
    int any = 0;
    while ((message = curl_multi_info_read(multi, &queued)))
      if (message->msg == CURLMSG_DONE) {
        answered(message->easy_handle, message->data.result);
        any = 1;
      }
    // One level deeper once every ask at this one is settled; the pages
    // put off till then have their links taken.
    while ((open_below == 0 && left > 0) || deferred_due) {
      if (!deferred_due) {
        level++;
        if (level < depth_room)
          open_below += open_at[level];
      }
      deferred_due = 0;
      int kept = 0;
      for (int i = 0; i < deferred_count; i++) {
        int id = deferred[i];
        if (asks[id].depth <= level) {
          extract_links(id);
          free(asks[id].body.p);
          asks[id].body = (Text){0};
          settle(&asks[id]);
        } else {
          deferred[kept++] = id;
        }
      }
      deferred_count = kept;
    }
    if (any)
      continue;
    double wait = next_due - now();
    int ms = wait <= 0 ? 0 : wait < 0.005 ? (int)(wait * 1000.0) + 1 : 5;
    if (running > 0) {
      curl_multi_poll(multi, NULL, 0, ms, NULL);
    } else if (ms > 0) {
      struct timespec pause = {0, ms * 1000000L};
      nanosleep(&pause, NULL);
    }
  }
  report();
  curl_multi_cleanup(multi);
  curl_global_cleanup();
  return 0;
}
