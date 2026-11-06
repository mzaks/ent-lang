// The crawl's own server (http-SPEC.md, §7.2): it answers every URL of a
// generated site as site.manifest says (site.py), as the mock does
// (mock.ent): the status, the type, the location of a redirect, the body,
// after the page's delay, and 503 for the first `fails` asks of a page.
// Any other URL is a 404. HTTP/1.1, keep-alive, on 127.0.0.1.
//
//   cc -O2 -o serve examples/http/serve.c -lpthread
//   serve [--threads N] [--port-file FILE] [--log FILE] site.manifest
//
// A request names its URL whole (as to a proxy: `GET http://site.test/p/
// 1.html HTTP/1.1`), so the crawler keeps the site's own URLs and goes
// through the device's `proxy`; or by its path and `Host`. The port it
// listens on is written to FILE (and said on stdout) once it does. It
// runs until SIGINT or SIGTERM; then, with --log, it writes a line for
// every request it answered: when it came and when its answer went (in
// seconds since the start), the URL and the status.
//
// Each thread has its own listening socket on the one port
// (SO_REUSEPORT) and its own epoll; an answer that is to wait is kept in
// the thread's heap of answers by when they are due.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  char *url;
  size_t url_length;
  int status, delay_ms, fails;
  char *kind, *location;
  char *body;
  size_t body_length;
  atomic_int asked;
} Page;

static Page *pages;
static size_t page_count, slots;
static int32_t *slot_page; // -1: empty

static uint64_t fnv(const char *s, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++)
    h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
  return h;
}

static Page *find_page(const char *url, size_t n) {
  for (size_t s = fnv(url, n) & (slots - 1);; s = (s + 1) & (slots - 1)) {
    int32_t p = slot_page[s];
    if (p < 0)
      return NULL;
    if (pages[p].url_length == n && !memcmp(pages[p].url, url, n))
      return &pages[p];
  }
}

static char *read_all(const char *name, size_t *length) {
  FILE *f = fopen(name, "rb");
  if (!f) {
    perror(name);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *data = malloc(n + 1);
  if (fread(data, 1, n, f) != (size_t)n) {
    perror(name);
    exit(1);
  }
  fclose(f);
  data[n] = 0;
  *length = n;
  return data;
}

// The next field of a manifest line, up to a space, made a string.
static char *field(char **at) {
  char *start = *at, *end = strchr(start, ' ');
  if (!end || end > strchr(start, '\n'))
    end = strchr(start, '\n');
  *end = 0;
  *at = end + 1;
  return start;
}

static void load(const char *name) {
  size_t length;
  char *all = read_all(name, &length);
  size_t room = 1024;
  pages = malloc(room * sizeof(Page));
  for (char *at = all; at < all + length;) {
    if (page_count == room)
      pages = realloc(pages, (room *= 2) * sizeof(Page));
    Page *p = &pages[page_count++];
    memset(p, 0, sizeof *p);
    p->url = field(&at);
    p->url_length = strlen(p->url);
    p->status = atoi(field(&at));
    p->delay_ms = atoi(field(&at));
    p->fails = atoi(field(&at));
    p->kind = field(&at);
    p->location = field(&at);
    if (!strcmp(p->location, "-"))
      p->location = NULL;
    p->body_length = strtoul(field(&at), NULL, 10);
    p->body = at;
    at += p->body_length + 1;
  }
  for (slots = 64; slots < page_count * 2; slots *= 2)
    ;
  slot_page = malloc(slots * sizeof(int32_t));
  memset(slot_page, 0xff, slots * sizeof(int32_t));
  for (size_t i = 0; i < page_count; i++) {
    size_t s = fnv(pages[i].url, pages[i].url_length) & (slots - 1);
    while (slot_page[s] >= 0)
      s = (s + 1) & (slots - 1);
    slot_page[s] = (int32_t)i;
  }
}

static double start_time;
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9 - start_time;
}

static const char *reason(int status) {
  switch (status) {
  case 200: return "OK";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 404: return "Not Found";
  case 500: return "Internal Server Error";
  case 503: return "Service Unavailable";
  default: return "Status";
  }
}

typedef struct {
  int fd;
  char *in;
  size_t in_length, in_room;
  char *out;
  size_t out_length, out_sent, out_room;
  int waiting; // answers due but not yet written
  int closing; // `Connection: close`: shut after what is to go
  uint32_t uses; // which connection on this fd, for the heap's entries
} Connection;

typedef struct {
  double due;
  int fd;
  uint32_t uses;
  Page *page; // NULL: 404
  int status, head;
  double came;
} Due;

typedef struct {
  char *text;
  size_t length, room;
} Log;

typedef struct {
  int listener, epoll;
  Connection *connections; // by fd
  int connection_room;
  Due *heap;
  size_t heap_count, heap_room;
  Log log;
} Thread;

static int port, logging;
static atomic_int stopping;

static void grow(char **buffer, size_t *room, size_t need) {
  if (need <= *room)
    return;
  while (*room < need)
    *room = *room ? *room * 2 : 4096;
  *buffer = realloc(*buffer, *room);
}

static void append(Connection *c, const char *data, size_t n) {
  grow(&c->out, &c->out_room, c->out_length + n);
  memcpy(c->out + c->out_length, data, n);
  c->out_length += n;
}

static void heap_push(Thread *t, Due d) {
  if (t->heap_count == t->heap_room) {
    t->heap_room = t->heap_room ? t->heap_room * 2 : 256;
    t->heap = realloc(t->heap, t->heap_room * sizeof(Due));
  }
  size_t i = t->heap_count++;
  while (i > 0 && t->heap[(i - 1) / 2].due > d.due) {
    t->heap[i] = t->heap[(i - 1) / 2];
    i = (i - 1) / 2;
  }
  t->heap[i] = d;
}

static Due heap_pop(Thread *t) {
  Due top = t->heap[0], last = t->heap[--t->heap_count];
  size_t i = 0;
  for (;;) {
    size_t child = 2 * i + 1;
    if (child >= t->heap_count)
      break;
    if (child + 1 < t->heap_count && t->heap[child + 1].due < t->heap[child].due)
      child++;
    if (t->heap[child].due >= last.due)
      break;
    t->heap[i] = t->heap[child];
    i = child;
  }
  if (t->heap_count)
    t->heap[i] = last;
  return top;
}

static Connection *connection(Thread *t, int fd) {
  if (fd >= t->connection_room) {
    int room = t->connection_room ? t->connection_room : 1024;
    while (room <= fd)
      room *= 2;
    t->connections = realloc(t->connections, room * sizeof(Connection));
    memset(t->connections + t->connection_room, 0,
           (room - t->connection_room) * sizeof(Connection));
    t->connection_room = room;
  }
  return &t->connections[fd];
}

static void close_connection(Thread *t, Connection *c) {
  epoll_ctl(t->epoll, EPOLL_CTL_DEL, c->fd, NULL);
  close(c->fd);
  c->in_length = c->out_length = c->out_sent = 0;
  c->waiting = c->closing = 0;
  c->uses++;
  c->fd = -1;
}

static void want_writes(Thread *t, Connection *c, int on) {
  struct epoll_event e = {.events = EPOLLIN | (on ? EPOLLOUT : 0),
                          .data.fd = c->fd};
  epoll_ctl(t->epoll, EPOLL_CTL_MOD, c->fd, &e);
}

// What is to go, as much as the socket takes; false where it is closed.
static int flush(Thread *t, Connection *c) {
  while (c->out_sent < c->out_length) {
    ssize_t n = send(c->fd, c->out + c->out_sent, c->out_length - c->out_sent,
                     MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        want_writes(t, c, 1);
        return 1;
      }
      close_connection(t, c);
      return 0;
    }
    c->out_sent += n;
  }
  c->out_sent = c->out_length = 0;
  want_writes(t, c, 0);
  if (c->closing && !c->waiting) {
    close_connection(t, c);
    return 0;
  }
  return 1;
}

static void write_answer(Thread *t, Due d) {
  Connection *c = connection(t, d.fd);
  if (c->fd != d.fd || c->uses != d.uses)
    return; // (closed since)
  char head[1024];
  const char *kind = d.page && d.status != 503 ? d.page->kind : "text/plain";
  const char *body = "not here";
  size_t length = 8;
  if (d.status == 503) {
    body = "try again";
    length = 9;
  } else if (d.page) {
    body = d.page->body;
    length = d.page->body_length;
  }
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n",
                   d.status, reason(d.status), kind, length);
  if (d.page && d.page->location && d.status != 503)
    n += snprintf(head + n, sizeof head - n, "Location: %s\r\n",
                  d.page->location);
  n += snprintf(head + n, sizeof head - n, "\r\n");
  append(c, head, n);
  if (!d.head)
    append(c, body, length);
  c->waiting--;
  if (logging) {
    char line[2304];
    int m = snprintf(line, sizeof line, "%.6f %.6f %.*s %d\n", d.came, now(),
                     d.page ? (int)d.page->url_length : 1,
                     d.page ? d.page->url : "-", d.status);
    grow(&t->log.text, &t->log.room, t->log.length + m);
    memcpy(t->log.text + t->log.length, line, m);
    t->log.length += m;
  }
  flush(t, c);
}

static int header_is(const char *line, size_t n, const char *name) {
  size_t k = strlen(name);
  return n > k && !strncasecmp(line, name, k);
}

// The requests that came whole: each answered now or put by when due.
static void take_requests(Thread *t, Connection *c) {
  size_t used = 0;
  for (;;) {
    char *start = c->in + used;
    size_t left = c->in_length - used;
    char *end = memmem(start, left, "\r\n\r\n", 4);
    if (!end)
      break;
    size_t head_length = end + 4 - start;
    // The request line: method, target, version.
    char *line_end = memmem(start, head_length, "\r\n", 2);
    char *space = memchr(start, ' ', line_end - start);
    if (!space) {
      close_connection(t, c);
      return;
    }
    char *target = space + 1;
    char *target_end = memchr(target, ' ', line_end - target);
    if (!target_end)
      target_end = line_end;
    int head = space - start == 4 && !memcmp(start, "HEAD", 4);
    // Its headers: Host, Content-Length, Connection.
    const char *host = NULL;
    size_t host_length = 0, body = 0;
    for (char *h = line_end + 2; h < end;) {
      char *h_end = memmem(h, end + 2 - h, "\r\n", 2);
      size_t n = h_end - h;
      if (header_is(h, n, "host:")) {
        host = h + 5;
        while (*host == ' ')
          host++;
        host_length = h_end - host;
      } else if (header_is(h, n, "content-length:")) {
        body = strtoul(h + 15, NULL, 10);
      } else if (header_is(h, n, "connection:") && memmem(h, n, "close", 5)) {
        c->closing = 1;
      }
      h = h_end + 2;
    }
    if (left < head_length + body)
      break;
    char url[2048];
    size_t url_length;
    if (*target == '/' && host) {
      url_length = snprintf(url, sizeof url, "http://%.*s%.*s",
                            (int)host_length, host,
                            (int)(target_end - target), target);
    } else {
      url_length = target_end - target;
      if (url_length >= sizeof url)
        url_length = sizeof url - 1;
      memcpy(url, target, url_length);
    }
    if (url_length >= sizeof url)
      url_length = sizeof url - 1;
    Page *page = find_page(url, url_length);
    Due d = {.fd = c->fd, .uses = c->uses, .page = page, .head = head,
             .came = now()};
    d.status = page ? page->status : 404;
    if (page && atomic_fetch_add(&page->asked, 1) < page->fails)
      d.status = 503;
    d.due = d.came + (page ? page->delay_ms / 1000.0 : 0.0);
    c->waiting++;
    if (page && page->delay_ms > 0)
      heap_push(t, d);
    else
      write_answer(t, d);
    if (c->fd < 0)
      return;
    used += head_length + body;
  }
  memmove(c->in, c->in + used, c->in_length - used);
  c->in_length -= used;
}

static void *serve(void *arg) {
  Thread *t = arg;
  struct epoll_event events[256];
  while (!atomic_load(&stopping)) {
    int wait = 100;
    if (t->heap_count) {
      double ms = (t->heap[0].due - now()) * 1000.0;
      wait = ms <= 0 ? 0 : ms < 100 ? (int)ms + 1 : 100;
    }
    int n = epoll_wait(t->epoll, events, 256, wait);
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == t->listener) {
        for (;;) {
          int client = accept4(t->listener, NULL, NULL, SOCK_NONBLOCK);
          if (client < 0)
            break;
          int one = 1;
          setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          Connection *c = connection(t, client);
          c->fd = client;
          c->in_length = c->out_length = c->out_sent = 0;
          c->waiting = c->closing = 0;
          struct epoll_event e = {.events = EPOLLIN, .data.fd = client};
          epoll_ctl(t->epoll, EPOLL_CTL_ADD, client, &e);
        }
        continue;
      }
      Connection *c = connection(t, fd);
      if (events[i].events & EPOLLOUT && !flush(t, c))
        continue;
      if (!(events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)))
        continue;
      for (;;) {
        grow(&c->in, &c->in_room, c->in_length + 65536);
        ssize_t got = recv(fd, c->in + c->in_length, c->in_room - c->in_length, 0);
        if (got > 0) {
          c->in_length += got;
          continue;
        }
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          close_connection(t, c);
          c = NULL;
        }
        break;
      }
      if (c)
        take_requests(t, c);
    }
    while (t->heap_count && t->heap[0].due <= now())
      write_answer(t, heap_pop(t));
  }
  return NULL;
}

static int listen_on(int at) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_port = htons(at),
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&address, sizeof address) ||
      listen(fd, 4096)) {
    perror("listen");
    exit(1);
  }
  socklen_t size = sizeof address;
  getsockname(fd, (struct sockaddr *)&address, &size);
  port = ntohs(address.sin_port);
  return fd;
}

static void on_signal(int signal) {
  (void)signal;
  atomic_store(&stopping, 1);
}

int main(int argc, char **argv) {
  int threads = 1;
  const char *port_file = NULL, *log_file = NULL, *manifest = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--threads") && i + 1 < argc)
      threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--port-file") && i + 1 < argc)
      port_file = argv[++i];
    else if (!strcmp(argv[i], "--log") && i + 1 < argc)
      log_file = argv[++i];
    else
      manifest = argv[i];
  }
  if (!manifest || threads < 1) {
    fprintf(stderr, "serve [--threads N] [--port-file FILE] [--log FILE] "
                    "site.manifest\n");
    return 2;
  }
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  start_time = t0.tv_sec + t0.tv_nsec * 1e-9;
  logging = log_file != NULL;
  load(manifest);
  struct sigaction action = {.sa_handler = on_signal};
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  signal(SIGPIPE, SIG_IGN);
  Thread *all = calloc(threads, sizeof(Thread));
  for (int i = 0; i < threads; i++) {
    all[i].listener = listen_on(i == 0 ? 0 : port);
    all[i].epoll = epoll_create1(0);
    struct epoll_event e = {.events = EPOLLIN, .data.fd = all[i].listener};
    epoll_ctl(all[i].epoll, EPOLL_CTL_ADD, all[i].listener, &e);
  }
  if (port_file) {
    char name[4096];
    snprintf(name, sizeof name, "%s.part", port_file);
    FILE *f = fopen(name, "w");
    fprintf(f, "%d\n", port);
    fclose(f);
    rename(name, port_file);
  }
  printf("port %d\n", port);
  fflush(stdout);
  pthread_t *running = calloc(threads, sizeof(pthread_t));
  for (int i = 0; i < threads; i++)
    pthread_create(&running[i], NULL, serve, &all[i]);
  for (int i = 0; i < threads; i++)
    pthread_join(running[i], NULL);
  if (log_file) {
    FILE *f = fopen(log_file, "w");
    for (int i = 0; i < threads; i++)
      fwrite(all[i].log.text, 1, all[i].log.length, f);
    fclose(f);
  }
  return 0;
}
