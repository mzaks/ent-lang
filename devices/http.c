// The C of the module `http` (devices/http.ent), on libcurl's multi
// interface: a multi handle for each session, an easy handle for each
// request while it runs. Requests are kept in a table, found by their
// handle: the row's number and a count of how often the row was used
// (so a handle forgotten is not taken for the next request in its row).
// Everything here runs on the program's main thread (procs do), except
// for a session put on a thread of its own (`http_on_thread`): that
// thread drives its multi handle (requests are added, moved on, removed
// there) and hands what is done back to the main thread, which does the
// rest. Rows and sessions stay where they are once made, so that thread
// may write a row's body and head while the main thread makes others.
#include "ent_extern.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { HTTP_OK, HTTP_REFUSED, HTTP_TIMEOUT, HTTP_DNS, HTTP_TLS, HTTP_TOO_BIG,
       HTTP_OTHER };

typedef struct {
  bool open;
  CURLM *multi;
  CURLSH *share;
  char *agent, *ca, *proxy;
  bool cookies, http2, verify;
  // No proxy is set in the environment (http_proxy, ...): curl is told
  // there is none, and does not look for one again at each request.
  bool no_proxy_set;
  int running;
  // Easy handles of requests done, reset, for the next (made anew costs
  // more than the request itself on a fast server).
  CURL **spare;
  int spare_count, spare_room;
  // On a thread of its own: the requests to add and to stop (rows), and
  // those done (rows and how they ended), between it and the main thread,
  // under `handover`; `stopping` ends the thread.
  bool threaded, stopping;
  pthread_t thread;
  int *adds, add_count, add_room;
  int *stops, stop_count, stop_room;
  int *done_rows, done_count, done_room;
  CURLcode *done_codes;
} http_session;

// Bytes that grow.
typedef struct {
  char *bytes;
  size_t length, room;
} http_bytes;

typedef struct {
  // 0: free; 1: running; 2: answered, waiting to be polled; 3: polled.
  int state;
  uint32_t uses;
  int session;
  CURL *easy;
  struct curl_slist *headers;
  http_bytes body, head;
  int64_t max_body;
  bool cut;
  int32_t status;
  uint8_t failure;
  double first_byte, total;
  char *url, *location;
} http_request;

// Sessions, each made where it stays; rows in blocks of 1024 that stay.
static http_session **sessions;
static int session_count;
enum { BLOCK_ROWS = 1024, MOST_BLOCKS = 65536 };
static http_request *blocks[MOST_BLOCKS];
static int request_count, request_room;
static inline http_request *row_at(intptr_t row) {
  return &blocks[row / BLOCK_ROWS][row % BLOCK_ROWS];
}
// What threads of sessions hand back, and the main thread waiting for it.
static pthread_mutex_t handover = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handed = PTHREAD_COND_INITIALIZER;
static int *free_rows;
static int free_count;
// The handles answered and not polled yet, in the order they were
// answered (one forgotten since is passed over).
static int64_t *answered;
static size_t answered_first, answered_count, answered_room;

static void *grown(void *at, size_t size) {
  void *made = realloc(at, size);
  if (!made && size)
    abort();
  return made;
}

static void bytes_add(http_bytes *to, const char *from, size_t length) {
  if (to->length + length + 1 > to->room) {
    size_t room = to->room ? to->room : 256;
    while (room < to->length + length + 1)
      room *= 2;
    to->bytes = grown(to->bytes, room);
    to->room = room;
  }
  memcpy(to->bytes + to->length, from, length);
  to->length += length;
}

static void bytes_free(http_bytes *bytes) {
  free(bytes->bytes);
  *bytes = (http_bytes){0};
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

static int64_t handle_of(int row) {
  return ((int64_t)row_at(row)->uses << 32) | (int64_t)(row + 1);
}

// The row of a handle, or -1 where it is none (or forgotten).
static int row_of(int64_t handle) {
  int64_t row = (handle & 0xffffffff) - 1;
  if (row < 0 || row >= request_count ||
      row_at(row)->uses != (uint32_t)(handle >> 32) ||
      row_at(row)->state == 0)
    return -1;
  return (int)row;
}

static void answer(int row) {
  if (answered_count == answered_room) {
    size_t room = answered_room ? answered_room * 2 : 64;
    int64_t *made = grown(NULL, room * sizeof(int64_t));
    for (size_t i = 0; i < answered_count; ++i)
      made[i] = answered[(answered_first + i) % answered_room];
    free(answered);
    answered = made;
    answered_first = 0;
    answered_room = room;
  }
  answered[(answered_first + answered_count++) % answered_room] =
      handle_of(row);
  row_at(row)->state = 2;
}

static size_t on_body(char *from, size_t size, size_t count, void *data) {
  http_request *request = row_at((intptr_t)data);
  size_t length = size * count;
  if (request->max_body > 0 &&
      request->body.length + length > (size_t)request->max_body) {
    bytes_add(&request->body, from,
              (size_t)request->max_body - request->body.length);
    request->cut = true;
    return 0;
  }
  bytes_add(&request->body, from, length);
  return length;
}

// Each line of each response's head: a status line starts the head of
// another (a redirect followed, a 100 Continue), whose lines replace
// those before; the empty line at its end is left out.
static size_t on_header(char *from, size_t size, size_t count, void *data) {
  http_request *request = row_at((intptr_t)data);
  size_t length = size * count;
  if (length >= 5 && memcmp(from, "HTTP/", 5) == 0)
    request->head.length = 0;
  else if (!(length == 2 && from[0] == '\r') && !(length == 1 && from[0] == '\n'))
    bytes_add(&request->head, from, length);
  return length;
}

static uint8_t failure_of(CURLcode code, bool cut) {
  switch (code) {
  case CURLE_OK:
  case CURLE_TOO_MANY_REDIRECTS:
    return HTTP_OK;
  case CURLE_WRITE_ERROR:
    return cut ? HTTP_OK : HTTP_OTHER;
  case CURLE_COULDNT_CONNECT:
    return HTTP_REFUSED;
  case CURLE_OPERATION_TIMEDOUT:
    return HTTP_TIMEOUT;
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY:
    return HTTP_DNS;
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_ISSUER_ERROR:
  case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
  case CURLE_SSL_INVALIDCERTSTATUS:
    return HTTP_TLS;
  case CURLE_FILESIZE_EXCEEDED:
    return HTTP_TOO_BIG;
  default:
    return HTTP_OTHER;
  }
}

static char *info_text(CURL *easy, CURLINFO what) {
  char *at = NULL;
  if (curl_easy_getinfo(easy, what, &at) != CURLE_OK || !at)
    return NULL;
  return strdup(at);
}

// An easy handle for a request of a session: one kept, or a new one.
static CURL *take_easy(http_session *session) {
  if (session->spare_count)
    return session->spare[--session->spare_count];
  return curl_easy_init();
}
// An easy handle no longer used (out of the multi handle), kept for the
// next request (at most 1024 a session).
static void give_easy(http_session *session, CURL *easy) {
  if (session->spare_count == 1024) {
    curl_easy_cleanup(easy);
    return;
  }
  if (session->spare_count == session->spare_room) {
    session->spare_room = session->spare_room ? session->spare_room * 2 : 64;
    session->spare = grown(session->spare, (size_t)session->spare_room * sizeof(CURL *));
  }
  curl_easy_reset(easy);
  session->spare[session->spare_count++] = easy;
}

// A request done: what it got kept, its easy handle given back.
static void finish(int row, CURLcode code) {
  http_request *request = row_at(row);
  CURL *easy = request->easy;
  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  curl_off_t first = 0, total = 0;
  curl_easy_getinfo(easy, CURLINFO_STARTTRANSFER_TIME_T, &first);
  curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME_T, &total);
  request->failure = failure_of(code, request->cut);
  request->status = request->failure == HTTP_OK ? (int32_t)status : 0;
  request->first_byte = (double)first / 1e6;
  request->total = (double)total / 1e6;
  request->url = info_text(easy, CURLINFO_EFFECTIVE_URL);
  if (request->status >= 300 && request->status < 400)
    request->location = info_text(easy, CURLINFO_REDIRECT_URL);
  // (On a thread of its own, the session's thread took it out already.)
  if (!sessions[request->session]->threaded)
    curl_multi_remove_handle(sessions[request->session]->multi, easy);
  give_easy(sessions[request->session], easy);
  request->easy = NULL;
  curl_slist_free_all(request->headers);
  request->headers = NULL;
  sessions[request->session]->running--;
  answer(row);
}

// A request on a session's thread, out of its multi handle (or never
// in it): what it has let go, its row free.
static void release(int row) {
  http_request *request = row_at(row);
  give_easy(sessions[request->session], request->easy);
  sessions[request->session]->running--;
  curl_slist_free_all(request->headers);
  bytes_free(&request->body);
  bytes_free(&request->head);
  free(request->url);
  free(request->location);
  uint32_t uses = request->uses;
  *request = (http_request){0};
  request->uses = uses;
  free_rows[free_count++] = row;
}

static void wake(http_session *session) { curl_multi_wakeup(session->multi); }

// A list of rows that grows (under `handover`).
static void row_add(int **rows, int *count, int *room, int row) {
  if (*count == *room) {
    *room = *room ? *room * 2 : 64;
    *rows = grown(*rows, (size_t)*room * sizeof(int));
  }
  (*rows)[(*count)++] = row;
}

static void forget(int row) {
  http_request *request = row_at(row);
  http_session *session = sessions[request->session];
  // Running on a session's thread: that thread takes it out of its multi
  // handle, and says so (then its row is let go); its handle is no
  // longer one now (state 4: stopping).
  if (request->easy && session->threaded && request->state == 1) {
    request->state = 4;
    request->uses++;
    pthread_mutex_lock(&handover);
    bool asleep = session->add_count == 0 && session->stop_count == 0;
    row_add(&session->stops, &session->stop_count, &session->stop_room, row);
    pthread_mutex_unlock(&handover);
    if (asleep)
      wake(session);
    return;
  }
  if (request->easy) {
    curl_multi_remove_handle(sessions[request->session]->multi, request->easy);
    give_easy(sessions[request->session], request->easy);
    sessions[request->session]->running--;
  }
  curl_slist_free_all(request->headers);
  bytes_free(&request->body);
  bytes_free(&request->head);
  free(request->url);
  free(request->location);
  uint32_t uses = request->uses;
  *request = (http_request){0};
  request->uses = uses + 1;
  free_rows[free_count++] = row;
}

static int new_row(void) {
  if (free_count)
    return free_rows[--free_count];
  if (request_count == request_room) {
    if (request_room / BLOCK_ROWS == MOST_BLOCKS)
      abort();
    blocks[request_room / BLOCK_ROWS] = grown(NULL, BLOCK_ROWS * sizeof(http_request));
    request_room += BLOCK_ROWS;
    free_rows = grown(free_rows, (size_t)request_room * sizeof(int));
  }
  *row_at(request_count) = (http_request){0};
  return request_count++;
}

int64_t ent_http_http_session(const ent_text *user_agent, bool cookies,
                              int32_t per_host, int32_t total, bool http2,
                              bool verify_tls, const ent_text *ca_file,
                              const ent_text *proxy) {
  static bool started;
  if (!started) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    started = true;
  }
  sessions = grown(sessions, (size_t)(session_count + 1) * sizeof(http_session *));
  http_session *session = grown(NULL, sizeof(http_session));
  sessions[session_count] = session;
  *session = (http_session){0};
  session->open = true;
  session->multi = curl_multi_init();
  if (per_host > 0)
    curl_multi_setopt(session->multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                      (long)per_host);
  if (total > 0)
    curl_multi_setopt(session->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                      (long)total);
  curl_multi_setopt(session->multi, CURLMOPT_PIPELINING,
                    http2 ? CURLPIPE_MULTIPLEX : CURLPIPE_NOTHING);
  session->share = curl_share_init();
  curl_share_setopt(session->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  curl_share_setopt(session->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  if (cookies)
    curl_share_setopt(session->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
  session->agent = copied(user_agent);
  session->ca = copied(ca_file);
  session->proxy = copied(proxy);
  session->cookies = cookies;
  session->http2 = http2;
  session->verify = verify_tls;
  static const char *const proxy_names[] = {
      "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
      "ALL_PROXY", "ftp_proxy", "FTP_PROXY", "ws_proxy", "wss_proxy"};
  session->no_proxy_set = true;
  for (size_t i = 0; i < sizeof proxy_names / sizeof *proxy_names; ++i) {
    const char *value = getenv(proxy_names[i]);
    if (value && value[0])
      session->no_proxy_set = false;
  }
  return ++session_count;
}

// The headers given as lines, each a header (an empty one left out).
static struct curl_slist *header_list(const ent_text *headers, bool *expect) {
  struct curl_slist *list = NULL;
  uint32_t length = headers ? headers->length : 0;
  uint32_t at = 0;
  while (at < length) {
    uint32_t end = at;
    while (end < length && headers->bytes[end] != '\n')
      ++end;
    uint32_t stop = end;
    if (stop > at && headers->bytes[stop - 1] == '\r')
      --stop;
    if (stop > at) {
      char *line = grown(NULL, stop - at + 1);
      memcpy(line, headers->bytes + at, stop - at);
      line[stop - at] = 0;
      if (strncasecmp(line, "expect:", 7) == 0)
        *expect = true;
      struct curl_slist *more = curl_slist_append(list, line);
      free(line);
      if (!more)
        abort();
      list = more;
    }
    at = end + 1;
  }
  return list;
}

int64_t ent_http_http_start(int64_t number, const ent_text *method,
                            const ent_text *url, const ent_text *headers,
                            const ent_text *body, double timeout,
                            int64_t max_body, int32_t follow) {
  int row = new_row();
  http_request *request = row_at(row);
  request->state = 1;
  request->session = (int)number - 1;
  request->max_body = max_body;
  if (number < 1 || number > session_count || !sessions[number - 1]->open) {
    request->session = 0;
    request->failure = HTTP_OTHER;
    answer(row);
    return handle_of(row);
  }
  http_session *session = sessions[request->session];
  CURL *easy = take_easy(session);
  request->easy = easy;
  void *data = (void *)(intptr_t)row;
  char *where = copied(url);
  curl_easy_setopt(easy, CURLOPT_URL, where);
  free(where);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, data);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, data);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_SHARE, session->share);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  if (session->agent[0])
    curl_easy_setopt(easy, CURLOPT_USERAGENT, session->agent);
  if (session->cookies)
    curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
                   session->http2 ? CURL_HTTP_VERSION_2TLS
                                  : CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, session->verify ? 1L : 0L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, session->verify ? 2L : 0L);
  if (session->ca[0])
    curl_easy_setopt(easy, CURLOPT_CAINFO, session->ca);
  if (session->proxy[0])
    curl_easy_setopt(easy, CURLOPT_PROXY, session->proxy);
  else if (session->no_proxy_set)
    curl_easy_setopt(easy, CURLOPT_PROXY, "");
  if (timeout > 0)
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)(timeout * 1000.0 + 0.5));
  if (follow > 0) {
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)follow);
  }
  // The method: GET and HEAD as curl has them, POST by its body (so a
  // 303 turns it into a GET, as browsers do; 307 and 308 keep it), any
  // other by name, with its body where it has one (or one of no bytes,
  // with its length, where it is PUT or PATCH).
  char *name = copied(method);
  uint32_t size = body ? body->length : 0;
  bool sends = size > 0 || strcmp(name, "POST") == 0 ||
               strcmp(name, "PUT") == 0 || strcmp(name, "PATCH") == 0;
  if (strcmp(name, "HEAD") == 0)
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
  else if (strcmp(name, "GET") == 0 && !size)
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
  else if (strcmp(name, "POST") != 0)
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, name);
  if (sends) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, size ? body->bytes : "");
  }
  free(name);
  bool expect = false;
  request->headers = header_list(headers, &expect);
  // (No waiting for a 100 Continue before a large body, unless asked.)
  if (!expect)
    request->headers = curl_slist_append(request->headers, "Expect:");
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request->headers);
  session->running++;
  if (session->threaded) {
    pthread_mutex_lock(&handover);
    bool asleep = session->add_count == 0 && session->stop_count == 0;
    row_add(&session->adds, &session->add_count, &session->add_room, row);
    pthread_mutex_unlock(&handover);
    if (asleep)
      wake(session);
    return handle_of(row);
  }
  if (curl_multi_add_handle(session->multi, easy) != CURLM_OK) {
    finish(row, CURLE_FAILED_INIT);
    return handle_of(row);
  }
  return handle_of(row);
}

void ent_http_http_cancel(int64_t handle) {
  int row = row_of(handle);
  if (row >= 0 && row_at(row)->state == 1)
    forget(row);
}

// A session's thread: requests added and stopped as the main thread
// asks, moved on, and those done handed back (rows, and how they ended;
// -1: stopped), until the session closes.
static void *drive(void *data) {
  http_session *session = data;
  int *adds = NULL, *stops = NULL, *rows = NULL;
  CURLcode *codes = NULL;
  int add_room = 0, stop_room = 0, done_room = 0;
  for (;;) {
    pthread_mutex_lock(&handover);
    bool stopping = session->stopping;
    int add_count = session->add_count, stop_count = session->stop_count;
    if (add_count > add_room) {
      add_room = session->add_room;
      adds = grown(adds, (size_t)add_room * sizeof(int));
    }
    if (stop_count > stop_room) {
      stop_room = session->stop_room;
      stops = grown(stops, (size_t)stop_room * sizeof(int));
    }
    if (add_count)
      memcpy(adds, session->adds, (size_t)add_count * sizeof(int));
    if (stop_count)
      memcpy(stops, session->stops, (size_t)stop_count * sizeof(int));
    session->add_count = session->stop_count = 0;
    pthread_mutex_unlock(&handover);
    if (stopping)
      break;
    int done = 0;
#define HAND_BACK(row_, code_)                                                 \
  do {                                                                         \
    if (done == done_room) {                                                   \
      done_room = done_room ? done_room * 2 : 64;                              \
      rows = grown(rows, (size_t)done_room * sizeof(int));                     \
      codes = grown(codes, (size_t)done_room * sizeof(CURLcode));              \
    }                                                                          \
    rows[done] = (row_);                                                       \
    codes[done++] = (code_);                                                   \
  } while (0)
    for (int i = 0; i < add_count; ++i)
      if (curl_multi_add_handle(session->multi, row_at(adds[i])->easy) != CURLM_OK)
        HAND_BACK(adds[i], CURLE_FAILED_INIT);
    for (int i = 0; i < stop_count; ++i) {
      curl_multi_remove_handle(session->multi, row_at(stops[i])->easy);
      HAND_BACK(stops[i], (CURLcode)-1);
    }
    int still = 0;
    curl_multi_perform(session->multi, &still);
    CURLMsg *message;
    int left = 0;
    while ((message = curl_multi_info_read(session->multi, &left))) {
      if (message->msg != CURLMSG_DONE)
        continue;
      void *row = NULL;
      CURL *easy = message->easy_handle;
      CURLcode code = message->data.result;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &row);
      curl_multi_remove_handle(session->multi, easy);
      HAND_BACK((int)(intptr_t)row, code);
    }
#undef HAND_BACK
    if (done) {
      pthread_mutex_lock(&handover);
      if (session->done_count + done > session->done_room) {
        while (session->done_count + done > session->done_room)
          session->done_room = session->done_room ? session->done_room * 2 : 64;
        session->done_rows =
            grown(session->done_rows, (size_t)session->done_room * sizeof(int));
        session->done_codes = grown(session->done_codes,
                                    (size_t)session->done_room * sizeof(CURLcode));
      }
      memcpy(session->done_rows + session->done_count, rows, (size_t)done * sizeof(int));
      memcpy(session->done_codes + session->done_count, codes,
             (size_t)done * sizeof(CURLcode));
      session->done_count += done;
      pthread_cond_broadcast(&handed);
      pthread_mutex_unlock(&handover);
    }
    // (Woken by the main thread where it has more to add or stop.)
    curl_multi_poll(session->multi, NULL, 0, 1000, NULL);
  }
  free(adds);
  free(stops);
  free(rows);
  free(codes);
  return NULL;
}

// What a session's thread handed back, taken in: done (answered), or
// stopped (let go).
static int *taken_rows;
static CURLcode *taken_codes;
static int taken_room;
static void collect(http_session *session) {
  pthread_mutex_lock(&handover);
  int count = session->done_count;
  if (count > taken_room) {
    taken_room = session->done_room;
    taken_rows = grown(taken_rows, (size_t)taken_room * sizeof(int));
    taken_codes = grown(taken_codes, (size_t)taken_room * sizeof(CURLcode));
  }
  if (count) {
    memcpy(taken_rows, session->done_rows, (size_t)count * sizeof(int));
    memcpy(taken_codes, session->done_codes, (size_t)count * sizeof(CURLcode));
  }
  session->done_count = 0;
  pthread_mutex_unlock(&handover);
  for (int i = 0; i < count; ++i) {
    int row = taken_rows[i];
    if (taken_codes[i] == (CURLcode)-1)
      release(row);
    else if (row_at(row)->state == 1)
      finish(row, taken_codes[i]);
  }
}

static bool handed_back(void) {
  for (int s = 0; s < session_count; ++s)
    if (sessions[s]->open && sessions[s]->threaded && sessions[s]->done_count)
      return true;
  return false;
}

bool ent_http_http_on_thread(int64_t number) {
  if (number < 1 || number > session_count || !sessions[number - 1]->open)
    return false;
  http_session *session = sessions[number - 1];
  if (session->threaded)
    return true;
  if (session->running)
    return false;
  session->threaded = true;
  if (pthread_create(&session->thread, NULL, drive, session) != 0) {
    session->threaded = false;
    return false;
  }
  return true;
}

// Each session's transfers moved on, and those done taken in.
static void perform(void) {
  for (int s = 0; s < session_count; ++s) {
    http_session *session = sessions[s];
    if (!session->open || !session->running)
      continue;
    if (session->threaded) {
      collect(session);
      continue;
    }
    int still = 0;
    curl_multi_perform(session->multi, &still);
    CURLMsg *message;
    int left = 0;
    while ((message = curl_multi_info_read(session->multi, &left))) {
      if (message->msg != CURLMSG_DONE)
        continue;
      void *data = NULL;
      curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &data);
      finish((int)(intptr_t)data, message->data.result);
    }
  }
}

int32_t ent_http_http_running(void) {
  int32_t running = 0;
  for (int s = 0; s < session_count; ++s)
    if (sessions[s]->open)
      running += sessions[s]->running;
  return running;
}

int32_t ent_http_http_poll(double wait, int32_t *count, int32_t capacity,
                           int64_t *handle, int32_t *status, uint8_t *failure,
                           double *first_byte, double *total, bool *cut) {
  perform();
  // Waiting: in the one session that runs requests, by curl; in several,
  // by turns of a millisecond each until one answers or the time is up.
  // Sessions on threads of their own: waited for by what they hand back.
  if (!answered_count && ent_http_http_running() > 0 && wait > 0) {
    int busy = 0, which = -1;
    for (int s = 0; s < session_count; ++s)
      if (sessions[s]->open && sessions[s]->running && !sessions[s]->threaded) {
        ++busy;
        which = s;
      }
    int ms = (int)(wait * 1000.0 + 0.5);
    if (busy == 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      long nanos = until.tv_nsec + (long)((wait - (double)(long)wait) * 1e9);
      until.tv_sec += (time_t)wait + nanos / 1000000000L;
      until.tv_nsec = nanos % 1000000000L;
      pthread_mutex_lock(&handover);
      while (!handed_back())
        if (pthread_cond_timedwait(&handed, &handover, &until) != 0)
          break;
      pthread_mutex_unlock(&handover);
      perform();
    } else if (busy == 1) {
      curl_multi_poll(sessions[which]->multi, NULL, 0, ms, NULL);
      perform();
    } else {
      for (int spent = 0; spent < ms && !answered_count; ) {
        for (int s = 0; s < session_count && !answered_count; ++s)
          if (sessions[s]->open && sessions[s]->running && !sessions[s]->threaded) {
            curl_multi_poll(sessions[s]->multi, NULL, 0, 1, NULL);
            ++spent;
          }
        perform();
      }
    }
  }
  int32_t given = 0;
  while (answered_count && given < capacity) {
    int row = row_of(answered[answered_first]);
    answered_first = (answered_first + 1) % answered_room;
    answered_count--;
    if (row < 0 || row_at(row)->state != 2)
      continue;
    http_request *request = row_at(row);
    request->state = 3;
    handle[given] = handle_of(row);
    status[given] = request->status;
    failure[given] = request->failure;
    first_byte[given] = request->first_byte;
    total[given] = request->total;
    cut[given] = request->cut;
    ++given;
  }
  *count = given;
  return given;
}

// What a request got, asked by its handle: -1 while it runs (or of a
// handle that is none).
int32_t ent_http_http_status(int64_t handle) {
  int row = row_of(handle);
  if (row < 0 || row_at(row)->state < 2)
    return -1;
  return row_at(row)->status;
}

uint8_t ent_http_http_failure(int64_t handle) {
  int row = row_of(handle);
  return row < 0 || row_at(row)->state < 2 ? HTTP_OK : row_at(row)->failure;
}

bool ent_http_http_cut(int64_t handle) {
  int row = row_of(handle);
  return row >= 0 && row_at(row)->state >= 2 && row_at(row)->cut;
}

double ent_http_http_total(int64_t handle) {
  int row = row_of(handle);
  return row < 0 || row_at(row)->state < 2 ? 0 : row_at(row)->total;
}

double ent_http_http_first_byte(int64_t handle) {
  int row = row_of(handle);
  return row < 0 || row_at(row)->state < 2 ? 0 : row_at(row)->first_byte;
}

const ent_text *ent_http_http_body(int64_t handle) {
  int row = row_of(handle);
  if (row < 0 || row_at(row)->state < 2)
    return NULL;
  return text_of(row_at(row)->body.bytes, row_at(row)->body.length);
}

const ent_text *ent_http_http_headers(int64_t handle) {
  int row = row_of(handle);
  if (row < 0 || row_at(row)->state < 2)
    return NULL;
  return text_of(row_at(row)->head.bytes, row_at(row)->head.length);
}

const ent_text *ent_http_http_url(int64_t handle) {
  int row = row_of(handle);
  if (row < 0 || row_at(row)->state < 2 || !row_at(row)->url)
    return NULL;
  return text_of(row_at(row)->url, strlen(row_at(row)->url));
}

const ent_text *ent_http_http_location(int64_t handle) {
  int row = row_of(handle);
  if (row < 0 || row_at(row)->state < 2 || !row_at(row)->location)
    return NULL;
  return text_of(row_at(row)->location, strlen(row_at(row)->location));
}

void ent_http_http_forget(int64_t handle) {
  int row = row_of(handle);
  if (row >= 0 && row_at(row)->state >= 2)
    forget(row);
}

void ent_http_http_close(int64_t number) {
  if (number < 1 || number > session_count || !sessions[number - 1]->open)
    return;
  int s = (int)number - 1;
  // Its thread stopped first: from then on the main thread has its multi
  // handle again, and what the thread did not hand back is let go below.
  if (sessions[s]->threaded) {
    pthread_mutex_lock(&handover);
    sessions[s]->stopping = true;
    pthread_mutex_unlock(&handover);
    wake(sessions[s]);
    pthread_join(sessions[s]->thread, NULL);
    sessions[s]->threaded = false;
    sessions[s]->add_count = sessions[s]->stop_count = sessions[s]->done_count = 0;
    free(sessions[s]->adds);
    free(sessions[s]->stops);
    free(sessions[s]->done_rows);
    free(sessions[s]->done_codes);
  }
  for (int row = 0; row < request_count; ++row)
    if (row_at(row)->session == s && row_at(row)->state != 0)
      forget(row);
  http_session *session = sessions[s];
  curl_multi_cleanup(session->multi);
  for (int i = 0; i < session->spare_count; ++i)
    curl_easy_cleanup(session->spare[i]);
  free(session->spare);
  session->spare = NULL;
  session->spare_count = session->spare_room = 0;
  curl_share_cleanup(session->share);
  free(session->agent);
  free(session->ca);
  free(session->proxy);
  session->open = false;
}
