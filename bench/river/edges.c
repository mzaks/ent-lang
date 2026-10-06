// Edge loops over a tree (see edges.ent): N nodes in a tree, and every
// node's sum the rain on the nodes that flow straight into it, gathered
// along the edges in (`pull`) or sent along the edges out (`push`).
//
//   VARIANT 0  c    by hand: for pull the nodes above each node next to
//                   each other (offsets per node), for push each node's
//                   parent
//   VARIANT 2  ent  the ent-lang program compiled into the binary, with
//                   its relation a tree (a slot per node, and links from
//                   child to child) or, `tree` taken out, a table of
//                   edges
//
// Shapes as in river.c: bushy, deep, shuffled. All variants add a node's
// inflows in the order of the nodes above it, so they agree to the bit.
// Compile with -ffp-contract=off.
//
// Usage: edges STEPS WARMUP SHAPE pull|push
// Prints: ns_per_step=... checksum=...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if VARIANT == 2
#include "edges_world.h"
#endif

#ifndef N
#error "define N"
#endif

static uint32_t state = 12345;
static uint32_t next(void) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static int32_t *parent;
static float *rain;

#if VARIANT == 0
static float *sum;
// The nodes above each node, by their numbers.
static int32_t *first, *above;

__attribute__((noinline)) static void pull(float wet) {
  for (int i = 0; i < N; ++i) {
    float total = 0.0f;
    for (int32_t at = first[i]; at < first[i + 1]; ++at)
      total += rain[above[at]] * wet;
    sum[i] = total;
  }
}

__attribute__((noinline)) static void push(float wet) {
  for (int i = 0; i < N; ++i)
    sum[i] = 0.0f;
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0)
      sum[parent[i]] += rain[i] * wet;
}
#endif

int main(int argc, char **argv) {
  int steps = argc > 1 ? atoi(argv[1]) : 100;
  int warmup = argc > 2 ? atoi(argv[2]) : 10;
  const char *shape = argc > 3 ? argv[3] : "bushy";
  int pulls = argc > 4 && strcmp(argv[4], "pull") == 0;

  parent = malloc(sizeof(int32_t) * N);
  rain = malloc(sizeof(float) * N);
  parent[0] = -1;
  for (int i = 1; i < N; ++i)
    parent[i] = strcmp(shape, "deep") == 0
                    ? i - 1 - (int32_t)(next() % (i < 8 ? i : 8))
                    : (int32_t)(next() % i);
  for (int i = 0; i < N; ++i)
    rain[i] = (float)(next() % 1000) / 1024.0f;
  if (strcmp(shape, "shuffled") == 0) {
    int32_t *to = malloc(sizeof(int32_t) * N);
    for (int i = 0; i < N; ++i)
      to[i] = i;
    for (int i = N - 1; i > 0; --i) {
      int32_t j = (int32_t)(next() % (i + 1)), t = to[i];
      to[i] = to[j];
      to[j] = t;
    }
    int32_t *moved = malloc(sizeof(int32_t) * N);
    float *wetter = malloc(sizeof(float) * N);
    for (int i = 0; i < N; ++i) {
      moved[to[i]] = parent[i] < 0 ? -1 : to[parent[i]];
      wetter[to[i]] = rain[i];
    }
    free(parent);
    free(rain);
    free(to);
    parent = moved;
    rain = wetter;
  }

#if VARIANT == 2
  ent_world *w = ent_world_create();
  if (!w) {
    fprintf(stderr, "cannot allocate the world\n");
    return 1;
  }
  ent_entity *ids = malloc(sizeof(ent_entity) * N);
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = ent_Cell_spawn(w);
    int64_t row = ent_entity_row(w, id);
    ent_Cell_Node_rain(w)[row] = rain[i];
    ent_Cell_Node_sum(w)[row] = 0.0f;
  }
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0 && !ent_Flows_connect(w, ids[i], ids[parent[i]]))
      return 1;
#define STEP(wet) (pulls ? ent_pull(w, wet) : ent_push(w, wet))
#define SUM(i) (ent_Cell_Node_sum(w)[ent_entity_row(w, ids[i])])
#else
  sum = calloc(N, sizeof(float));
  first = calloc(N + 1, sizeof(int32_t));
  above = malloc(sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0)
      ++first[parent[i] + 1];
  for (int i = 0; i < N; ++i)
    first[i + 1] += first[i];
  int32_t *cursor = malloc(sizeof(int32_t) * N);
  memcpy(cursor, first, sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0)
      above[cursor[parent[i]]++] = i;
  free(cursor);
#define STEP(wet) (pulls ? pull(wet) : push(wet))
#define SUM(i) (sum[i])
#endif

  for (int s = 0; s < warmup; ++s)
    STEP(1.0f + (float)(s % 7) * 0.125f);
  double start = now();
  for (int s = 0; s < steps; ++s)
    STEP(1.0f + (float)(s % 7) * 0.125f);
  double elapsed = now() - start;

  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < N; ++i) {
    float f = SUM(i);
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    hash = (hash ^ bits) * 1099511628211ull;
  }
  printf("ns_per_step=%.0f checksum=%016llx\n", elapsed / steps,
         (unsigned long long)hash);
  return 0;
}
