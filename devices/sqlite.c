// The sqlite device: a query's rows of a database opened read-only, as
// text (devices/sqlite.ent).
#include "ent_extern.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static char *failure;

static void failed(const char *why) {
  free(failure);
  failure = why ? strdup(why) : NULL;
}

static char *c_text(const ent_text *t) {
  uint32_t n = t ? t->length : 0;
  char *s = malloc(n + 1);
  memcpy(s, t ? t->bytes : "", n);
  s[n] = 0;
  return s;
}

typedef struct {
  char *bytes;
  size_t length, room;
} grown_bytes;

static void put(grown_bytes *b, const char *bytes, size_t n) {
  if (b->length + n > b->room) {
    b->room = (b->length + n) * 2 + 256;
    b->bytes = realloc(b->bytes, b->room);
  }
  memcpy(b->bytes + b->length, bytes, n);
  b->length += n;
}

static void put_escaped(grown_bytes *b, const unsigned char *v, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (v[i] == '\t')
      put(b, "\\t", 2);
    else if (v[i] == '\n')
      put(b, "\\n", 2);
    else if (v[i] == '\\')
      put(b, "\\\\", 2);
    else
      put(b, (const char *)v + i, 1);
  }
}

const ent_text *ent_sqlite_sqlite_rows(const ent_text *file,
                                       const ent_text *query) {
  failed(NULL);
  char *path = c_text(file), *sql = c_text(query);
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  grown_bytes out = {0};
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    failed(db ? sqlite3_errmsg(db) : "cannot open the file");
  } else if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    failed(sqlite3_errmsg(db));
  } else {
    int step;
    int columns = sqlite3_column_count(stmt);
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
      for (int c = 0; c < columns; ++c) {
        if (c > 0)
          put(&out, "\t", 1);
        if (sqlite3_column_type(stmt, c) == SQLITE_NULL)
          continue;
        const unsigned char *v = sqlite3_column_type(stmt, c) == SQLITE_BLOB
                                     ? sqlite3_column_blob(stmt, c)
                                     : sqlite3_column_text(stmt, c);
        put_escaped(&out, v, (size_t)sqlite3_column_bytes(stmt, c));
      }
      put(&out, "\n", 1);
    }
    if (step != SQLITE_DONE)
      failed(sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  free(path);
  free(sql);
  ent_text *made = NULL;
  if (out.length && !failure) {
    made = ent_text_new((uint32_t)out.length);
    memcpy(made->bytes, out.bytes, out.length);
  }
  free(out.bytes);
  return made;
}

const ent_text *ent_sqlite_sqlite_failure(void) {
  if (!failure)
    return 0;
  size_t n = strlen(failure);
  ent_text *made = ent_text_new((uint32_t)n);
  memcpy(made->bytes, failure, n);
  return made;
}
