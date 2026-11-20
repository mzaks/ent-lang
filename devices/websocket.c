// The C of the module `websocket` (devices/websocket.ent): libcurl opens
// the connection (TCP, TLS, a proxy's tunnel) and stops there
// (CURLOPT_CONNECT_ONLY, driven by a multi handle so that nothing
// waits); the upgrade and the frames (RFC 6455) are written and read
// here, over curl_easy_send and curl_easy_recv. Connections are kept in
// a table, found by their handle: the row's number and a count of how
// often the row was used. Everything runs on the program's main thread.
#include "ent_extern.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

// The most a message may have (more: the connection is closed, 1009).
enum { WS_MOST = 64 * 1024 * 1024 };
enum { WS_OPENED, WS_TEXT, WS_BINARY, WS_CLOSED, WS_FAILED };
// A connection's state: free; connecting (curl); upgrading (the request
// sent, the answer read); open; closing (a close sent, the server's
// awaited); done.
enum { ST_FREE, ST_CONNECTING, ST_UPGRADING, ST_OPEN, ST_CLOSING, ST_DONE };

typedef struct {
  char *bytes;
  size_t length, room;
} ws_bytes;

typedef struct {
  int state;
  uint32_t uses;
  CURL *easy;
  bool in_multi;
  char *host_line, *target, *headers;
  char key[25];
  ws_bytes in, out, message, answer_head;
  int message_opcode;
  double opened, deadline;
  char *failure;
} ws_conn;

typedef struct {
  int64_t socket;
  uint8_t event;
  int32_t code;
  double at;
  char *data;
  size_t length;
} ws_event;

static ws_conn **conns;
static int conn_count, conn_room;
static CURLM *multi;
// Events not polled yet, and those of the last poll (their bytes kept
// until the next).
static ws_event *queued, *polled;
static int queued_count, queued_room, polled_count;

static void *grown(void *at, size_t size) {
  void *made = realloc(at, size);
  if (!made && size)
    abort();
  return made;
}
static void bytes_add(ws_bytes *to, const void *from, size_t length) {
  if (to->length + length + 1 > to->room) {
    size_t room = to->room ? to->room : 256;
    while (room < to->length + length + 1)
      room *= 2;
    to->bytes = grown(to->bytes, room);
    to->room = room;
  }
  memcpy(to->bytes + to->length, from, length);
  to->length += length;
  to->bytes[to->length] = 0;
}
static void bytes_drop(ws_bytes *b, size_t n) {
  memmove(b->bytes, b->bytes + n, b->length - n);
  b->length -= n;
}
static void bytes_free(ws_bytes *b) {
  free(b->bytes);
  *b = (ws_bytes){0};
}
static char *copied(const ent_text *text) {
  uint32_t length = text ? text->length : 0;
  char *made = grown(NULL, length + 1);
  memcpy(made, text ? text->bytes : "", length);
  made[length] = 0;
  return made;
}
static const ent_text *text_of(const char *bytes, size_t length) {
  if (!bytes || !length)
    return NULL;
  ent_text *text = ent_text_new((uint32_t)length);
  memcpy(text->bytes, bytes, length);
  return text;
}
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void random_bytes(unsigned char *to, size_t n) {
  static int fd = -2;
  if (fd == -2)
    fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0 && read(fd, to, n) == (ssize_t)n)
    return;
  static uint64_t x = 0;
  if (!x)
    x = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ull | 1;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    to[i] = (unsigned char)x;
  }
}

// --- SHA-1 and base64, for the handshake's key and its answer. ---

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1(const unsigned char *data, size_t length, unsigned char out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  size_t total = ((length + 8) / 64 + 1) * 64;
  unsigned char *m = calloc(total, 1);
  memcpy(m, data, length);
  m[length] = 0x80;
  uint64_t bits = (uint64_t)length * 8;
  for (int i = 0; i < 8; ++i)
    m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
  for (size_t at = 0; at < total; at += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)m[at + 4 * i] << 24 | (uint32_t)m[at + 4 * i + 1] << 16 |
             (uint32_t)m[at + 4 * i + 2] << 8 | m[at + 4 * i + 3];
    for (int i = 16; i < 80; ++i)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  free(m);
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j)
      out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}
static void base64(const unsigned char *in, size_t n, char *out) {
  static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                 (i + 2 < n ? in[i + 2] : 0);
    out[o++] = a[v >> 18 & 63];
    out[o++] = a[v >> 12 & 63];
    out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? a[v & 63] : '=';
  }
  out[o] = 0;
}

// --- The table. ---

static int64_t handle_of(int row) {
  return ((int64_t)conns[row]->uses << 32) | (int64_t)(row + 1);
}
static int row_of(int64_t handle) {
  int64_t row = (handle & 0xffffffff) - 1;
  if (row < 0 || row >= conn_count || conns[row]->uses != (uint32_t)(handle >> 32) ||
      conns[row]->state == ST_FREE)
    return -1;
  return (int)row;
}
static void say(int row, int event, int32_t code, const char *data, size_t length) {
  if (queued_count == queued_room) {
    queued_room = queued_room ? queued_room * 2 : 64;
    queued = grown(queued, (size_t)queued_room * sizeof(ws_event));
  }
  ws_event *e = &queued[queued_count++];
  e->socket = handle_of(row);
  e->event = (uint8_t)event;
  e->code = code;
  e->at = now() - conns[row]->opened;
  e->length = length;
  e->data = NULL;
  if (length) {
    e->data = grown(NULL, length);
    memcpy(e->data, data, length);
  }
}
// The transport let go (the row stays, done, until forgotten).
static void let_go(ws_conn *c) {
  if (c->easy) {
    if (c->in_multi)
      curl_multi_remove_handle(multi, c->easy);
    curl_easy_cleanup(c->easy);
  }
  c->easy = NULL;
  c->in_multi = false;
}
static void fail(int row, int32_t code, const char *why) {
  ws_conn *c = conns[row];
  free(c->failure);
  c->failure = strdup(why);
  let_go(c);
  c->state = ST_DONE;
  say(row, WS_FAILED, code, why, strlen(why));
}
static void closed(int row, int32_t code, const char *reason, size_t length) {
  ws_conn *c = conns[row];
  let_go(c);
  c->state = ST_DONE;
  say(row, WS_CLOSED, code, reason, length);
}

// --- Sending. ---

// What is waiting to be sent, as much as the socket takes now.
static bool flush(int row) {
  ws_conn *c = conns[row];
  while (c->out.length) {
    size_t sent = 0;
    CURLcode r = curl_easy_send(c->easy, c->out.bytes, c->out.length, &sent);
    if (r == CURLE_AGAIN)
      return true;
    if (r != CURLE_OK)
      return false;
    bytes_drop(&c->out, sent);
  }
  return true;
}
static void frame(ws_conn *c, int opcode, const char *data, size_t length) {
  unsigned char head[14];
  size_t n = 0;
  head[n++] = (unsigned char)(0x80 | opcode);
  if (length < 126)
    head[n++] = (unsigned char)(0x80 | length);
  else if (length < 65536) {
    head[n++] = 0x80 | 126;
    head[n++] = (unsigned char)(length >> 8);
    head[n++] = (unsigned char)length;
  } else {
    head[n++] = 0x80 | 127;
    for (int i = 7; i >= 0; --i)
      head[n++] = (unsigned char)((uint64_t)length >> (8 * i));
  }
  unsigned char mask[4];
  random_bytes(mask, 4);
  memcpy(head + n, mask, 4);
  n += 4;
  bytes_add(&c->out, head, n);
  size_t at = c->out.length;
  bytes_add(&c->out, data, length);
  for (size_t i = 0; i < length; ++i)
    c->out.bytes[at + i] ^= mask[i % 4];
}
static void close_frame(ws_conn *c, int32_t code, const char *reason, size_t length) {
  char payload[125];
  size_t n = 0;
  if (code > 0) {
    payload[n++] = (char)(code >> 8);
    payload[n++] = (char)code;
    size_t r = length > 123 ? 123 : length;
    memcpy(payload + n, reason, r);
    n += r;
  }
  frame(c, 8, payload, n);
}

// --- Reading. ---

// The handshake's answer, once whole: 101 with the right accept, else
// failed.
static void upgrade_answer(int row) {
  ws_conn *c = conns[row];
  char *end = strstr(c->in.bytes ? c->in.bytes : "", "\r\n\r\n");
  if (!end)
    return;
  size_t head_length = (size_t)(end - c->in.bytes) + 4;
  int status = 0;
  sscanf(c->in.bytes, "HTTP/%*s %d", &status);
  char *first = strstr(c->in.bytes, "\r\n");
  bytes_add(&c->answer_head, first + 2, head_length - (size_t)(first + 2 - c->in.bytes) - 2);
  // The accept the key asks for.
  char joined[64];
  snprintf(joined, sizeof joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", c->key);
  unsigned char digest[20];
  sha1((const unsigned char *)joined, strlen(joined), digest);
  char want[32];
  base64(digest, 20, want);
  bool accepted = false;
  for (char *line = c->answer_head.bytes; line && *line;) {
    char *eol = strstr(line, "\r\n");
    size_t n = eol ? (size_t)(eol - line) : strlen(line);
    if (n > 21 && strncasecmp(line, "sec-websocket-accept:", 21) == 0) {
      char *v = line + 21;
      while (*v == ' ' || *v == '\t')
        ++v;
      size_t vn = n - (size_t)(v - line);
      while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t'))
        --vn;
      accepted = vn == strlen(want) && strncmp(v, want, vn) == 0;
    }
    line = eol ? eol + 2 : NULL;
  }
  bytes_drop(&c->in, head_length);
  if (status != 101) {
    char why[32];
    snprintf(why, sizeof why, "status %d", status);
    fail(row, status, why);
  } else if (!accepted)
    fail(row, status, "no upgrade");
  else {
    c->state = ST_OPEN;
    say(row, WS_OPENED, 101, NULL, 0);
  }
}
// The frames read whole: messages said, pings answered, a close
// answered and said.
static void frames(int row) {
  ws_conn *c = conns[row];
  while (c->state == ST_OPEN || c->state == ST_CLOSING) {
    unsigned char *b = (unsigned char *)c->in.bytes;
    size_t have = c->in.length;
    if (have < 2)
      return;
    bool fin = b[0] & 0x80;
    int opcode = b[0] & 0x0f;
    bool masked = b[1] & 0x80;
    uint64_t length = b[1] & 0x7f;
    size_t at = 2;
    if (length == 126) {
      if (have < 4)
        return;
      length = (uint64_t)b[2] << 8 | b[3];
      at = 4;
    } else if (length == 127) {
      if (have < 10)
        return;
      length = 0;
      for (int i = 0; i < 8; ++i)
        length = length << 8 | b[2 + i];
      at = 10;
    }
    if (length > WS_MOST || c->message.length + length > WS_MOST) {
      close_frame(c, 1009, "too big", 7);
      flush(row);
      closed(row, 1009, "message too big", 15);
      return;
    }
    unsigned char mask[4] = {0};
    if (masked) {
      if (have < at + 4)
        return;
      memcpy(mask, b + at, 4);
      at += 4;
    }
    if (have < at + length)
      return;
    char *payload = (char *)b + at;
    if (masked)
      for (uint64_t i = 0; i < length; ++i)
        payload[i] ^= (char)mask[i % 4];
    size_t n = (size_t)length;
    if (opcode == 9) {
      frame(c, 10, payload, n);
    } else if (opcode == 8) {
      int32_t code = n >= 2 ? ((unsigned char)payload[0] << 8 | (unsigned char)payload[1]) : 1005;
      const char *reason = n > 2 ? payload + 2 : "";
      size_t reason_length = n > 2 ? n - 2 : 0;
      if (c->state == ST_OPEN)
        close_frame(c, n >= 2 ? code : 0, "", 0);
      flush(row);
      // (The reason copied before the row's bytes go.)
      char *kept = grown(NULL, reason_length + 1);
      memcpy(kept, reason, reason_length);
      bytes_drop(&c->in, at + n);
      closed(row, code, kept, reason_length);
      free(kept);
      return;
    } else if (opcode == 1 || opcode == 2 || opcode == 0) {
      if (opcode != 0)
        c->message_opcode = opcode;
      bytes_add(&c->message, payload, n);
      if (fin) {
        say(row, c->message_opcode == 2 ? WS_BINARY : WS_TEXT, 0, c->message.bytes, c->message.length);
        c->message.length = 0;
      }
    }
    bytes_drop(&c->in, at + n);
  }
}
// What the socket has, read; the connection moved on.
static void read_all(int row) {
  ws_conn *c = conns[row];
  char chunk[16384];
  for (;;) {
    size_t got = 0;
    CURLcode r = curl_easy_recv(c->easy, chunk, sizeof chunk, &got);
    if (r == CURLE_AGAIN)
      break;
    if (r != CURLE_OK || got == 0) {
      if (c->state == ST_UPGRADING)
        fail(row, 0, "closed before the upgrade");
      else if (c->state == ST_OPEN || c->state == ST_CLOSING)
        closed(row, 1006, "connection lost", 15);
      return;
    }
    bytes_add(&c->in, chunk, got);
    if (c->state == ST_UPGRADING)
      upgrade_answer(row);
    if (c->state == ST_OPEN || c->state == ST_CLOSING)
      frames(row);
    if (c->state == ST_DONE)
      return;
  }
}

static const char *failure_of(CURLcode r) {
  switch (r) {
  case CURLE_COULDNT_CONNECT: return "refused";
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY: return "dns";
  case CURLE_OPERATION_TIMEDOUT: return "timeout";
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_SSL_CACERT_BADFILE: return "tls";
  default: return "other";
  }
}

// Connections moved on: those curl connected get the upgrade sent, the
// open ones are read, the late ones fail or close.
static void progress(void) {
  if (multi) {
    int running = 0;
    curl_multi_perform(multi, &running);
    CURLMsg *m;
    int left = 0;
    while ((m = curl_multi_info_read(multi, &left))) {
      if (m->msg != CURLMSG_DONE)
        continue;
      intptr_t row = 0;
      curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&row);
      ws_conn *c = conns[row];
      if (c->state != ST_CONNECTING)
        continue;
      if (m->data.result != CURLE_OK) {
        fail((int)row, 0, failure_of(m->data.result));
        continue;
      }
      char *request = NULL;
      size_t length = 0;
      FILE *f = open_memstream(&request, &length);
      fprintf(f, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
              c->target, c->host_line, c->key, c->headers);
      fclose(f);
      bytes_add(&c->out, request, length);
      free(request);
      c->state = ST_UPGRADING;
    }
  }
  double t = now();
  for (int row = 0; row < conn_count; ++row) {
    ws_conn *c = conns[row];
    if (c->state < ST_UPGRADING || c->state == ST_DONE)
      continue;
    if (!flush(row)) {
      if (c->state == ST_UPGRADING)
        fail(row, 0, "closed before the upgrade");
      else
        closed(row, 1006, "connection lost", 15);
      continue;
    }
    read_all(row);
    if (c->state == ST_UPGRADING && c->deadline > 0 && t > c->deadline)
      fail(row, 0, "timeout");
    else if (c->state == ST_CLOSING && t > c->deadline)
      closed(row, 1006, "no close from the server", 24);
  }
}

// --- The procs. ---

// "Name: value" lines, each ended by "\r\n" (lines ended by "\n" too).
static char *header_lines(const ent_text *headers) {
  char *raw = copied(headers);
  ws_bytes out = {0};
  for (char *line = raw; *line;) {
    char *eol = strchr(line, '\n');
    size_t n = eol ? (size_t)(eol - line) : strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
      --n;
    if (n && memchr(line, ':', n)) {
      bytes_add(&out, line, n);
      bytes_add(&out, "\r\n", 2);
    }
    line = eol ? eol + 1 : line + n;
  }
  free(raw);
  if (!out.bytes)
    bytes_add(&out, "", 0);
  return out.bytes;
}

int64_t ent_websocket_ws_open(const ent_text *url, const ent_text *headers, double timeout,
                              bool verify_tls, const ent_text *ca_file, const ent_text *proxy) {
  static bool started;
  if (!started) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    started = true;
  }
  if (!multi)
    multi = curl_multi_init();
  int row = -1;
  for (int r = 0; r < conn_count && row < 0; ++r)
    if (conns[r]->state == ST_FREE)
      row = r;
  if (row < 0) {
    if (conn_count == conn_room) {
      conn_room = conn_room ? conn_room * 2 : 16;
      conns = grown(conns, (size_t)conn_room * sizeof(ws_conn *));
    }
    conns[conn_count] = calloc(1, sizeof(ws_conn));
    row = conn_count++;
  }
  ws_conn *c = conns[row];
  uint32_t uses = c->uses + 1;
  *c = (ws_conn){0};
  c->uses = uses;
  c->state = ST_CONNECTING;
  c->opened = now();
  c->deadline = timeout > 0 ? c->opened + timeout : 0;
  unsigned char key[16];
  random_bytes(key, 16);
  base64(key, 16, c->key);
  c->headers = header_lines(headers);
  // ws:// as http://, wss:// as https:// for curl; the host and the
  // target for the request line.
  char *given = copied(url);
  const char *rest = given;
  bool secure = false;
  if (strncasecmp(given, "wss://", 6) == 0) { secure = true; rest = given + 6; }
  else if (strncasecmp(given, "ws://", 5) == 0) rest = given + 5;
  else if (strncasecmp(given, "https://", 8) == 0) { secure = true; rest = given + 8; }
  else if (strncasecmp(given, "http://", 7) == 0) rest = given + 7;
  size_t host_length = strcspn(rest, "/?#");
  c->host_line = strndup(rest, host_length);
  const char *path = rest + host_length;
  size_t path_length = strcspn(path, "#");
  if (path_length == 0 || path[0] != '/') {
    c->target = grown(NULL, path_length + 2);
    c->target[0] = '/';
    memcpy(c->target + 1, path, path_length);
    c->target[path_length + 1] = 0;
  } else
    c->target = strndup(path, path_length);
  char *curl_url = NULL;
  size_t curl_url_length = 0;
  FILE *f = open_memstream(&curl_url, &curl_url_length);
  fprintf(f, "%s://%s/", secure ? "https" : "http", c->host_line);
  fclose(f);
  free(given);

  c->easy = curl_easy_init();
  if (!c->easy || !host_length) {
    free(curl_url);
    fail(row, 0, host_length ? "other" : "no host");
    return handle_of(row);
  }
  CURL *e = c->easy;
  curl_easy_setopt(e, CURLOPT_URL, curl_url);
  free(curl_url);
  curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(e, CURLOPT_PRIVATE, (char *)(intptr_t)row);
  curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(e, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(e, CURLOPT_SSL_ENABLE_ALPN, 0L);
  curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, verify_tls ? 1L : 0L);
  curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, verify_tls ? 2L : 0L);
  char *ca = copied(ca_file);
  if (ca[0])
    curl_easy_setopt(e, CURLOPT_CAINFO, ca);
  free(ca);
  char *through = copied(proxy);
  curl_easy_setopt(e, CURLOPT_PROXY, through);
  if (through[0])
    curl_easy_setopt(e, CURLOPT_HTTPPROXYTUNNEL, 1L);
  free(through);
  if (timeout > 0)
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout * 1000.0 + 0.5));
  curl_multi_add_handle(multi, e);
  c->in_multi = true;
  return handle_of(row);
}

bool ent_websocket_ws_send(int64_t socket, const ent_text *data, bool binary) {
  int row = row_of(socket);
  if (row < 0 || conns[row]->state != ST_OPEN)
    return false;
  frame(conns[row], binary ? 2 : 1, data ? data->bytes : "", data ? data->length : 0);
  if (!flush(row))
    closed(row, 1006, "connection lost", 15);
  return true;
}

void ent_websocket_ws_close(int64_t socket, int32_t code, const ent_text *reason) {
  int row = row_of(socket);
  if (row < 0)
    return;
  ws_conn *c = conns[row];
  if (c->state == ST_OPEN) {
    close_frame(c, code, reason ? reason->bytes : "", reason ? reason->length : 0);
    c->state = ST_CLOSING;
    c->deadline = now() + 2.0;
    if (!flush(row))
      closed(row, code, reason ? reason->bytes : "", reason ? reason->length : 0);
  } else if (c->state == ST_CONNECTING || c->state == ST_UPGRADING)
    closed(row, code, reason ? reason->bytes : "", reason ? reason->length : 0);
}

int32_t ent_websocket_ws_poll(double wait, int32_t *count, int32_t capacity, int64_t *socket,
                              uint8_t *event, int32_t *code, double *at) {
  for (int i = 0; i < polled_count; ++i)
    free(polled[i].data);
  polled_count = 0;
  progress();
  double until = now() + wait;
  while (!queued_count && wait > 0) {
    double left = until - now();
    if (left <= 0)
      break;
    int busy = 0;
    struct curl_waitfd fds[64];
    unsigned nfds = 0;
    for (int row = 0; row < conn_count; ++row) {
      ws_conn *c = conns[row];
      if (c->state == ST_CONNECTING)
        ++busy;
      else if (c->state >= ST_UPGRADING && c->state <= ST_CLOSING) {
        ++busy;
        curl_socket_t s = CURL_SOCKET_BAD;
        if (nfds < 64 && curl_easy_getinfo(c->easy, CURLINFO_ACTIVESOCKET, &s) == CURLE_OK &&
            s != CURL_SOCKET_BAD) {
          fds[nfds].fd = s;
          fds[nfds].events = CURL_WAIT_POLLIN;
          fds[nfds].revents = 0;
          ++nfds;
        }
      }
    }
    if (!busy)
      break;
    int ms = (int)(left * 1000.0);
    if (ms > 50)
      ms = 50;
    if (ms < 1)
      ms = 1;
    curl_multi_poll(multi, fds, nfds, ms, NULL);
    progress();
  }
  int32_t given = 0;
  polled = grown(polled, (size_t)(capacity > 0 ? capacity : 1) * sizeof(ws_event));
  while (given < queued_count && given < capacity) {
    ws_event *e = &queued[given];
    socket[given] = e->socket;
    event[given] = e->event;
    code[given] = e->code;
    at[given] = e->at;
    polled[given] = *e;
    ++given;
  }
  polled_count = given;
  memmove(queued, queued + given, (size_t)(queued_count - given) * sizeof(ws_event));
  queued_count -= given;
  *count = given;
  return given;
}

const ent_text *ent_websocket_ws_event_data(int32_t i) {
  if (i < 0 || i >= polled_count)
    return NULL;
  return text_of(polled[i].data, polled[i].length);
}

const ent_text *ent_websocket_ws_answer_headers(int64_t socket) {
  int row = row_of(socket);
  if (row < 0)
    return NULL;
  return text_of(conns[row]->answer_head.bytes, conns[row]->answer_head.length);
}

const ent_text *ent_websocket_ws_failure(int64_t socket) {
  int row = row_of(socket);
  if (row < 0 || !conns[row]->failure)
    return NULL;
  return text_of(conns[row]->failure, strlen(conns[row]->failure));
}

bool ent_websocket_ws_is_open(int64_t socket) {
  int row = row_of(socket);
  return row >= 0 && conns[row]->state == ST_OPEN;
}

void ent_websocket_ws_forget(int64_t socket) {
  int row = row_of(socket);
  if (row < 0)
    return;
  ws_conn *c = conns[row];
  if (c->state == ST_OPEN) {
    close_frame(c, 1001, "", 0);
    flush(row);
  }
  let_go(c);
  bytes_free(&c->in);
  bytes_free(&c->out);
  bytes_free(&c->message);
  bytes_free(&c->answer_head);
  free(c->host_line);
  free(c->target);
  free(c->headers);
  free(c->failure);
  uint32_t uses = c->uses;
  *c = (ws_conn){0};
  c->uses = uses;
  c->state = ST_FREE;
}
