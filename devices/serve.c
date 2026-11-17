// The C of the module `serve` (devices/serve.ent): an HTTP/1.1 server on
// plain sockets and poll(2), on the program's thread. A server is a
// listening socket; each connection accepted is a row of a table, found
// by a handle (the row's number and how often the row was used, so that
// a handle kept after its connection closed is not taken for another).
// A row reads until a whole request is there, is handed to the program
// (`serve_next`), waits for its answer, writes it, then reads the next
// request where the client keeps the connection.
#include "ent_extern.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int64_t ent_serve_serve_open(const ent_text *address, int32_t port) {
  (void)address, (void)port;
  return 0;
}
int32_t ent_serve_serve_port(int64_t server) {
  (void)server;
  return 0;
}
int64_t ent_serve_serve_next(int64_t server, double wait) {
  (void)server, (void)wait;
  return 0;
}
const ent_text *ent_serve_serve_method(int64_t asked) {
  (void)asked;
  return NULL;
}
const ent_text *ent_serve_serve_target(int64_t asked) {
  (void)asked;
  return NULL;
}
const ent_text *ent_serve_serve_headers(int64_t asked) {
  (void)asked;
  return NULL;
}
const ent_text *ent_serve_serve_body(int64_t asked) {
  (void)asked;
  return NULL;
}
bool ent_serve_serve_answer(int64_t asked, int32_t status, const ent_text *headers,
                            const ent_text *body) {
  (void)asked, (void)status, (void)headers, (void)body;
  return false;
}
void ent_serve_serve_close(int64_t server) { (void)server; }

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// A request's head, and its body, at most so big.
enum { HEAD_MOST = 65536 };
#define BODY_MOST ((size_t)256 << 20)

typedef struct {
  char *bytes;
  size_t length, room;
} serve_bytes;

static void bytes_add(serve_bytes *b, const char *bytes, size_t length) {
  if (b->length + length + 1 > b->room) {
    size_t room = b->room ? b->room : 1024;
    while (room < b->length + length + 1)
      room *= 2;
    b->bytes = realloc(b->bytes, room);
    b->room = room;
  }
  if (length)
    memcpy(b->bytes + b->length, bytes, length);
  b->length += length;
  b->bytes[b->length] = 0;
}
static void bytes_text(serve_bytes *b, const char *text) { bytes_add(b, text, strlen(text)); }
static void bytes_free(serve_bytes *b) {
  free(b->bytes);
  *b = (serve_bytes){0};
}

// A connection: reading (1), its request asked (2), its answer being
// written (3); free (0).
enum { FREE, READING, ASKED, WRITING };
typedef struct {
  int state;
  int fd, server;
  uint32_t uses;
  serve_bytes in, out;
  size_t out_at;
  // The request read: where it ends in `in`, and its parts.
  size_t used;
  bool handed, keep, head, continued;
  serve_bytes method, target, headers, body;
} serve_row;

typedef struct {
  bool open;
  int fd, port;
} serve_server;

static serve_row *rows;
static int row_count;
static serve_server *servers;
static int server_count;

static int64_t handle_of(int row) { return ((int64_t)rows[row].uses << 32) | (int64_t)(row + 1); }
static int row_of(int64_t handle) {
  int row = (int)(handle & 0xffffffff) - 1;
  if (row < 0 || row >= row_count || rows[row].state == FREE ||
      rows[row].uses != (uint32_t)(handle >> 32))
    return -1;
  return row;
}
static serve_server *server_of(int64_t number) {
  if (number < 1 || number > server_count || !servers[number - 1].open)
    return NULL;
  return &servers[number - 1];
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static const ent_text *text_of(const char *bytes, size_t length) {
  if (!bytes || !length)
    return NULL;
  ent_text *text = ent_text_new((uint32_t)length);
  memcpy(text->bytes, bytes, length);
  return text;
}

static void request_clear(serve_row *r) {
  r->method.length = r->target.length = r->headers.length = r->body.length = 0;
  r->used = 0;
  r->handed = r->head = r->continued = false;
}

static void row_close(int row) {
  serve_row *r = &rows[row];
  close(r->fd);
  bytes_free(&r->in);
  bytes_free(&r->out);
  bytes_free(&r->method);
  bytes_free(&r->target);
  bytes_free(&r->headers);
  bytes_free(&r->body);
  r->state = FREE;
  r->uses++;
}

static const char *reason(int status) {
  switch (status) {
  case 100: return "Continue";
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 413: return "Content Too Large";
  case 422: return "Unprocessable Content";
  case 429: return "Too Many Requests";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default: return "";
  }
}

// Writes what is still to write of a row's answer, as much as the socket
// takes: 1 all of it, 0 not yet, -1 the client gone.
static int row_flush(serve_row *r) {
  while (r->out_at < r->out.length) {
    ssize_t n = send(r->fd, r->out.bytes + r->out_at, r->out.length - r->out_at, MSG_NOSIGNAL);
    if (n > 0)
      r->out_at += (size_t)n;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;
    else if (n < 0 && errno == EINTR)
      continue;
    else
      return -1;
  }
  return 1;
}

static bool head_is(const char *line, size_t length, const char *name) {
  size_t n = strlen(name);
  return length > n && line[n] == ':' && strncasecmp(line, name, n) == 0;
}
static const char *head_value(const char *line, size_t length, size_t *value_length) {
  const char *colon = memchr(line, ':', length);
  const char *v = colon + 1, *end = line + length;
  while (v < end && (*v == ' ' || *v == '\t'))
    v++;
  while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
    end--;
  *value_length = (size_t)(end - v);
  return v;
}
static bool value_has(const char *v, size_t length, const char *word) {
  size_t n = strlen(word);
  for (size_t i = 0; i + n <= length; i++)
    if (strncasecmp(v + i, word, n) == 0)
      return true;
  return false;
}

static void answer_now(serve_row *r, int status, bool keep);

// Reads a whole request from what came, where it is all there: 1 it is,
// 0 not yet, -1 it cannot be (answered with an error, then closed).
static int row_parse(serve_row *r) {
  if (r->state != READING)
    return 0;
  char *in = r->in.bytes;
  size_t have = r->in.length;
  if (!have)
    return 0;
  // (Empty lines before a request are let be, as RFC 9112 says.)
  size_t start = 0;
  while (start + 1 < have && in[start] == '\r' && in[start + 1] == '\n')
    start += 2;
  char *end = NULL;
  for (size_t i = start; i + 3 < have; i++)
    if (in[i] == '\r' && in[i + 1] == '\n' && in[i + 2] == '\r' && in[i + 3] == '\n') {
      end = in + i;
      break;
    }
  if (!end) {
    if (have - start > HEAD_MOST) {
      answer_now(r, 431, false);
      return -1;
    }
    return 0;
  }
  bool was_continued = r->continued;
  request_clear(r);
  // The request line: method, target, version.
  char *line = in + start;
  char *line_end = memchr(line, '\r', (size_t)(end + 2 - line));
  char *sp1 = memchr(line, ' ', (size_t)(line_end - line));
  char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1)) : NULL;
  if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1 || line_end - sp2 < 9 ||
      strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
    answer_now(r, 400, false);
    return -1;
  }
  bytes_add(&r->method, line, (size_t)(sp1 - line));
  bytes_add(&r->target, sp1 + 1, (size_t)(sp2 - sp1 - 1));
  bool old = sp2[8] == '0';
  r->keep = !old;
  r->head = strcmp(r->method.bytes, "HEAD") == 0;
  // The headers.
  long long length = -1;
  bool chunked = false, expect = false;
  char *at = line_end + 2;
  while (at < end + 2) {
    char *e = memchr(at, '\r', (size_t)(end + 2 - at));
    if (!e || e == at)
      break;
    size_t n = (size_t)(e - at), vn;
    if (memchr(at, ':', n)) {
      bytes_add(&r->headers, at, n);
      bytes_text(&r->headers, "\r\n");
      const char *v = head_value(at, n, &vn);
      if (head_is(at, n, "Content-Length")) {
        char number[32];
        snprintf(number, sizeof number, "%.*s", (int)(vn < 31 ? vn : 31), v);
        char *stop;
        length = strtoll(number, &stop, 10);
        if (*stop || length < 0) {
          answer_now(r, 400, false);
          return -1;
        }
      } else if (head_is(at, n, "Transfer-Encoding")) {
        chunked = value_has(v, vn, "chunked");
      } else if (head_is(at, n, "Connection")) {
        if (value_has(v, vn, "close"))
          r->keep = false;
        else if (value_has(v, vn, "keep-alive"))
          r->keep = true;
      } else if (head_is(at, n, "Expect")) {
        expect = value_has(v, vn, "100-continue");
      }
    }
    at = e + 2;
  }
  size_t body_at = (size_t)(end + 4 - in);
  // The body: so long, or in chunks, or none.
  if (chunked) {
    size_t p = body_at;
    for (;;) {
      char *crlf = NULL;
      for (size_t i = p; i + 1 < have; i++)
        if (in[i] == '\r' && in[i + 1] == '\n') {
          crlf = in + i;
          break;
        }
      if (!crlf)
        goto more;
      char *stop;
      unsigned long long size = strtoull(in + p, &stop, 16);
      if (stop == in + p) {
        answer_now(r, 400, false);
        return -1;
      }
      p = (size_t)(crlf + 2 - in);
      if (size == 0) {
        // (Trailers, let be, to the empty line.)
        for (;;) {
          char *e = NULL;
          for (size_t i = p; i + 1 < have; i++)
            if (in[i] == '\r' && in[i + 1] == '\n') {
              e = in + i;
              break;
            }
          if (!e)
            goto more;
          bool empty = e == in + p;
          p = (size_t)(e + 2 - in);
          if (empty)
            break;
        }
        r->used = p;
        break;
      }
      if (r->body.length + size > BODY_MOST) {
        answer_now(r, 413, false);
        return -1;
      }
      if (have < p + size + 2)
        goto more;
      bytes_add(&r->body, in + p, (size_t)size);
      p += (size_t)size + 2;
    }
  } else if (length > 0) {
    if ((size_t)length > BODY_MOST) {
      answer_now(r, 413, false);
      return -1;
    }
    if (have < body_at + (size_t)length)
      goto more;
    bytes_add(&r->body, in + body_at, (size_t)length);
    r->used = body_at + (size_t)length;
  } else {
    r->used = body_at;
  }
  r->state = ASKED;
  return 1;
more:
  // The body is still to come: where the client waits to be told to
  // send it, it is told.
  if (expect && !was_continued) {
    const char *go = "HTTP/1.1 100 Continue\r\n\r\n";
    send(r->fd, go, strlen(go), MSG_NOSIGNAL);
  }
  request_clear(r);
  r->continued = expect;
  return 0;
}

// A row's answer written: the next request read where the connection is
// kept, else the row closed.
static void row_written(int row) {
  serve_row *r = &rows[row];
  if (!r->keep) {
    row_close(row);
    return;
  }
  size_t used = r->used;
  memmove(r->in.bytes, r->in.bytes + used, r->in.length - used);
  r->in.length -= used;
  r->out.length = r->out_at = 0;
  request_clear(r);
  r->state = READING;
  if (row_parse(r) < 0 && r->state == WRITING && row_flush(r) != 0)
    row_close(row);
}

static void answer_put(serve_row *r, int status, const char *headers, size_t headers_length,
                       const char *body, size_t body_length) {
  r->out.length = r->out_at = 0;
  char line[96];
  snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, reason(status));
  bytes_text(&r->out, line);
  // The program's headers, each ended by "\r\n", but those the server says.
  size_t i = 0;
  while (i < headers_length) {
    const char *h = headers + i;
    const char *nl = memchr(h, '\n', headers_length - i);
    size_t n = nl ? (size_t)(nl - h) : headers_length - i;
    i += n + (nl ? 1 : 0);
    if (n && h[n - 1] == '\r')
      n--;
    if (!n || !memchr(h, ':', n) || head_is(h, n, "Content-Length") ||
        head_is(h, n, "Connection") || head_is(h, n, "Transfer-Encoding"))
      continue;
    bytes_add(&r->out, h, n);
    bytes_text(&r->out, "\r\n");
  }
  bool bodiless = status == 204 || status == 304 || (status >= 100 && status < 200);
  if (!bodiless) {
    snprintf(line, sizeof line, "Content-Length: %zu\r\n", body_length);
    bytes_text(&r->out, line);
  }
  bytes_text(&r->out, r->keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
  if (!bodiless && !r->head)
    bytes_add(&r->out, body, body_length);
  r->state = WRITING;
}

// An error answered by the server itself, the connection closed after.
static void answer_now(serve_row *r, int status, bool keep) {
  r->keep = keep;
  r->head = false;
  answer_put(r, status, "", 0, "", 0);
}

static void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

int64_t ent_serve_serve_open(const ent_text *address, int32_t port) {
  char host[256];
  snprintf(host, sizeof host, "%.*s", address ? (int)address->length : 0,
           address ? (const char *)address->bytes : "");
  struct sockaddr_in at = {0};
  at.sin_family = AF_INET;
  at.sin_port = htons((uint16_t)port);
  if (!host[0])
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  else if (inet_pton(AF_INET, host, &at.sin_addr) != 1)
    return 0;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  if (bind(fd, (struct sockaddr *)&at, sizeof at) != 0 || listen(fd, 128) != 0) {
    close(fd);
    return 0;
  }
  set_nonblocking(fd);
  socklen_t size = sizeof at;
  getsockname(fd, (struct sockaddr *)&at, &size);
  int n = 0;
  while (n < server_count && servers[n].open)
    n++;
  if (n == server_count) {
    servers = realloc(servers, sizeof *servers * (size_t)(server_count + 1));
    server_count++;
  }
  servers[n] = (serve_server){true, fd, ntohs(at.sin_port)};
  return n + 1;
}

int32_t ent_serve_serve_port(int64_t number) {
  serve_server *s = server_of(number);
  return s ? s->port : 0;
}

static void accept_all(int number, int fd) {
  for (;;) {
    int c = accept(fd, NULL, NULL);
    if (c < 0)
      return;
    set_nonblocking(c);
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    int row = 0;
    while (row < row_count && rows[row].state != FREE)
      row++;
    if (row == row_count) {
      rows = realloc(rows, sizeof *rows * (size_t)(row_count + 1));
      rows[row_count] = (serve_row){0};
      row_count++;
    }
    uint32_t uses = rows[row].uses;
    rows[row] = (serve_row){0};
    rows[row].uses = uses;
    rows[row].state = READING;
    rows[row].fd = c;
    rows[row].server = number;
  }
}

static int64_t ready(int number) {
  for (int row = 0; row < row_count; row++)
    if (rows[row].state == ASKED && rows[row].server == number && !rows[row].handed) {
      rows[row].handed = true;
      return handle_of(row);
    }
  return 0;
}

int64_t ent_serve_serve_next(int64_t number, double wait) {
  serve_server *s = server_of(number);
  if (!s)
    return 0;
  double until = now() + (wait > 0 ? wait : 0);
  struct pollfd *polled = NULL;
  int *polled_rows = NULL;
  int64_t found = 0;
  for (;;) {
    if ((found = ready((int)number)))
      break;
    int count = 0;
    polled = realloc(polled, sizeof *polled * (size_t)(row_count + 1));
    polled_rows = realloc(polled_rows, sizeof *polled_rows * (size_t)(row_count + 1));
    polled[count] = (struct pollfd){s->fd, POLLIN, 0};
    polled_rows[count++] = -1;
    for (int row = 0; row < row_count; row++) {
      serve_row *r = &rows[row];
      if (r->server != number || (r->state != READING && r->state != WRITING))
        continue;
      polled[count] = (struct pollfd){r->fd, r->state == READING ? POLLIN : POLLOUT, 0};
      polled_rows[count++] = row;
    }
    double left = until - now();
    int ms = left > 0 ? (int)(left * 1000.0 + 0.999) : 0;
    int n = poll(polled, (nfds_t)count, ms);
    if (n < 0 && errno != EINTR)
      break;
    for (int k = 0; n > 0 && k < count; k++) {
      if (!polled[k].revents)
        continue;
      if (polled_rows[k] < 0) {
        accept_all((int)number, s->fd);
        continue;
      }
      int row = polled_rows[k];
      serve_row *r = &rows[row];
      if (r->state == READING) {
        char chunk[65536];
        ssize_t got = recv(r->fd, chunk, sizeof chunk, 0);
        if (got > 0) {
          bytes_add(&r->in, chunk, (size_t)got);
          if (row_parse(r) < 0 && row_flush(r) != 0)
            row_close(row);
        } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          row_close(row);
        }
      } else if (r->state == WRITING) {
        int done = row_flush(r);
        if (done > 0)
          row_written(row);
        else if (done < 0)
          row_close(row);
      }
    }
    if ((found = ready((int)number)) || now() >= until)
      break;
  }
  free(polled);
  free(polled_rows);
  return found;
}

static const ent_text *part(int64_t asked, int which) {
  int row = row_of(asked);
  if (row < 0 || rows[row].state != ASKED)
    return NULL;
  serve_bytes *b = which == 0   ? &rows[row].method
                   : which == 1 ? &rows[row].target
                   : which == 2 ? &rows[row].headers
                                : &rows[row].body;
  return text_of(b->bytes, b->length);
}
const ent_text *ent_serve_serve_method(int64_t asked) { return part(asked, 0); }
const ent_text *ent_serve_serve_target(int64_t asked) { return part(asked, 1); }
const ent_text *ent_serve_serve_headers(int64_t asked) { return part(asked, 2); }
const ent_text *ent_serve_serve_body(int64_t asked) { return part(asked, 3); }

bool ent_serve_serve_answer(int64_t asked, int32_t status, const ent_text *headers,
                            const ent_text *body) {
  int row = row_of(asked);
  if (row < 0 || rows[row].state != ASKED || !rows[row].handed)
    return false;
  serve_row *r = &rows[row];
  answer_put(r, status, headers ? (const char *)headers->bytes : "", headers ? headers->length : 0,
             body ? (const char *)body->bytes : "", body ? body->length : 0);
  int done = row_flush(r);
  if (done > 0)
    row_written(row);
  else if (done < 0)
    row_close(row);
  return true;
}

void ent_serve_serve_close(int64_t number) {
  serve_server *s = server_of(number);
  if (!s)
    return;
  for (int row = 0; row < row_count; row++)
    if (rows[row].state != FREE && rows[row].server == number)
      row_close(row);
  close(s->fd);
  s->open = false;
}

#endif
