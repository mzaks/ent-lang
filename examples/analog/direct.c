// Way 0's solve (direct.ent): the entries of a matrix and its right-hand
// side, added up where they meet, and solved with KLU (SuiteSparse).
//
// An entry is a row, a column and a value; a column of -1 is the
// right-hand side's. The matrix is put together column by column
// (compressed sparse columns, which KLU takes). Where its pattern is the
// one of the call before, which is so for every Newton step and time point
// of one circuit, KLU's analysis of it is kept and only the numbers are
// factored again.
//
// Gives 0 where it solved; 1 where the matrix is singular (a node with
// nothing to hold its voltage, say); 2 where the solution has no room in
// `into`, or an entry is outside the matrix; 3 where KLU failed otherwise
// (memory).
#include "ent_extern.h"

#include <suitesparse/klu.h>
#include <stdlib.h>
#include <string.h>

static struct {
  int size;
  int *starts, *rows;   // the pattern: a column's rows from starts[col]
  double *values;
  int count;            // entries in the pattern
  klu_symbolic *symbolic;
  klu_numeric *numeric;
  klu_common common;
} kept;

static int *counts, *place;
static int countsRoom, placeRoom;

static void forget(void) {
  if (kept.numeric)
    klu_free_numeric(&kept.numeric, &kept.common);
  if (kept.symbolic)
    klu_free_symbolic(&kept.symbolic, &kept.common);
  free(kept.starts);
  free(kept.rows);
  free(kept.values);
  memset(&kept, 0, sizeof kept);
}

static int compareInts(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

int32_t ent_direct_solve_entries(int32_t count, const int32_t *row, const int32_t *col,
                                 const double *value, int32_t size, int32_t *into_count,
                                 int32_t into_capacity, double *into) {
  *into_count = 0;
  if (size <= 0 || size > into_capacity)
    return 2;
  for (int32_t k = 0; k < count; ++k)
    if (row[k] < 0 || row[k] >= size || col[k] < -1 || col[k] >= size)
      return 2;
  // The pattern: for each column its rows, in order, once each.
  if (countsRoom < size + 1) {
    free(counts);
    counts = malloc(sizeof(int) * (size_t)(size + 1));
    countsRoom = size + 1;
  }
  if (placeRoom < count + 1) {
    free(place);
    place = malloc(sizeof(int) * (size_t)(count + 1));
    placeRoom = count + 1;
  }
  int *starts = malloc(sizeof(int) * (size_t)(size + 1));
  int *rows = malloc(sizeof(int) * (size_t)(count + 1));
  if (!counts || !place || !starts || !rows) {
    free(starts);
    free(rows);
    return 3;
  }
  memset(counts, 0, sizeof(int) * (size_t)(size + 1));
  for (int32_t k = 0; k < count; ++k)
    if (col[k] >= 0)
      counts[col[k] + 1] += 1;
  for (int c = 0; c < size; ++c)
    counts[c + 1] += counts[c];
  // (Every entry's row in its column, then each column's rows sorted and
  // the same ones made one.)
  int *fill = malloc(sizeof(int) * (size_t)(size + 1));
  if (!fill) {
    free(starts);
    free(rows);
    return 3;
  }
  memcpy(fill, counts, sizeof(int) * (size_t)(size + 1));
  for (int32_t k = 0; k < count; ++k)
    if (col[k] >= 0)
      rows[fill[col[k]]++] = row[k];
  int nonzeros = 0;
  for (int c = 0; c < size; ++c) {
    int from = counts[c], to = counts[c + 1];
    qsort(rows + from, (size_t)(to - from), sizeof(int), compareInts);
    starts[c] = nonzeros;
    for (int k = from; k < to; ++k)
      if (k == from || rows[k] != rows[k - 1])
        rows[nonzeros++] = rows[k];
  }
  starts[size] = nonzeros;
  free(fill);

  int same = kept.symbolic && kept.size == size && kept.count == nonzeros &&
             memcmp(kept.starts, starts, sizeof(int) * (size_t)(size + 1)) == 0 &&
             memcmp(kept.rows, rows, sizeof(int) * (size_t)nonzeros) == 0;
  if (!same) {
    forget();
    klu_defaults(&kept.common);
    kept.size = size;
    kept.count = nonzeros;
    kept.starts = starts;
    kept.rows = rows;
    kept.values = malloc(sizeof(double) * (size_t)(nonzeros + 1));
    if (!kept.values)
      return 3;
    kept.symbolic = klu_analyze(size, kept.starts, kept.rows, &kept.common);
    if (!kept.symbolic)
      return 3;
  } else {
    free(starts);
    free(rows);
  }

  // The values, added up where entries meet, and the right-hand side.
  memset(kept.values, 0, sizeof(double) * (size_t)nonzeros);
  memset(into, 0, sizeof(double) * (size_t)size);
  for (int32_t k = 0; k < count; ++k) {
    if (col[k] < 0) {
      into[row[k]] += value[k];
      continue;
    }
    // (Where the row is in its column: they are in order.)
    int lo = kept.starts[col[k]], hi = kept.starts[col[k] + 1] - 1;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (kept.rows[mid] < row[k])
        lo = mid + 1;
      else
        hi = mid;
    }
    kept.values[lo] += value[k];
  }

  int factored;
  if (kept.numeric)
    factored = klu_refactor(kept.starts, kept.rows, kept.values, kept.symbolic,
                            kept.numeric, &kept.common);
  else
    factored = (kept.numeric = klu_factor(kept.starts, kept.rows, kept.values,
                                          kept.symbolic, &kept.common)) != 0;
  if (!factored || kept.common.status != KLU_OK) {
    int singular = kept.common.status == KLU_SINGULAR;
    if (kept.numeric)
      klu_free_numeric(&kept.numeric, &kept.common);
    return singular ? 1 : 3;
  }
  if (!klu_solve(kept.symbolic, kept.numeric, size, 1, into, &kept.common))
    return kept.common.status == KLU_SINGULAR ? 1 : 3;
  *into_count = size;
  return 0;
}
