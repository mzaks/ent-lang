// A scene graph (scene.ent): N nodes in a tree, each placed at its
// parent's place plus its own offset. Every step the host moves MOVES of
// them, and the program places the nodes again: all of them, or with a
// reactive `for` the moved ones and what is below them.
//
//   scene <steps> <warmup> <bushy|deep> <moves>
//
// Prints the time per step, how many nodes hang below a moved one on
// average (itself included), and a checksum of every node's place.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scene_world.h"

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

int main(int argc, char **argv) {
  int steps = argc > 1 ? atoi(argv[1]) : 100;
  int warmup = argc > 2 ? atoi(argv[2]) : 10;
  const char *shape = argc > 3 ? argv[3] : "bushy";
  int moves = argc > 4 ? atoi(argv[4]) : 1;
  if (moves > 1024)
    moves = 1024;

  int32_t *parent = malloc(sizeof(int32_t) * N);
  parent[0] = -1;
  for (int i = 1; i < N; ++i)
    parent[i] = strcmp(shape, "deep") == 0
                    ? i - 1 - (int32_t)(next() % (i < 8 ? i : 8))
                    : (int32_t)(next() % i);
  // How many nodes hang below each, itself included: a parent has a
  // smaller number than its children.
  int64_t *below = malloc(sizeof(int64_t) * N);
  for (int i = 0; i < N; ++i)
    below[i] = 1;
  for (int i = N - 1; i > 0; --i)
    below[parent[i]] += below[i];

  ent_world *w = ent_world_create();
  if (!w) {
    fprintf(stderr, "cannot allocate the world\n");
    return 1;
  }
  ent_entity *ids = malloc(sizeof(ent_entity) * N);
#ifdef TWO
  // Half of the nodes in a second archetype, by a hash of the number.
  #define HEAVY(i) (((uint32_t)(i) * 2654435761u >> 16) & 1)
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = HEAVY(i) ? ent_Heavy_spawn(w) : ent_Node_spawn(w);
    int64_t row = ent_entity_row(w, id);
    int32_t x = (int32_t)(next() % 1000);
    if (HEAVY(i)) {
      ent_Heavy_Local_x(w)[row] = x;
      ent_Heavy_World_x(w)[row] = 0;
      ent_Heavy_Mass_m(w)[row] = 0;
    } else {
      ent_Node_Local_x(w)[row] = x;
      ent_Node_World_x(w)[row] = 0;
    }
  }
  #define WORLD(i)                                                            \
    ((HEAVY(i) ? ent_Heavy_World_x(w)                                         \
               : ent_Node_World_x(w))[ent_entity_row(w, ids[i])])
#else
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = ent_Node_spawn(w);
    int64_t row = ent_entity_row(w, id);
    ent_Node_Local_x(w)[row] = (int32_t)(next() % 1000);
    ent_Node_World_x(w)[row] = 0;
  }
  #define WORLD(i) (ent_Node_World_x(w)[ent_entity_row(w, ids[i])])
#endif
  for (int i = 1; i < N; ++i)
    if (!ent_Under_connect(w, ids[i], ids[parent[i]]))
      return 1;
  ent_entity *picks = malloc(sizeof(ent_entity) * (moves ? moves : 1));
  for (int m = 0; m < moves; ++m)
    picks[m] = ent_Picks_spawn(w);

  double reached = 0.0;
  double elapsed = 0.0;
  for (int s = 0; s < warmup + steps; ++s) {
    for (int m = 0; m < moves; ++m) {
      // Any node but the first, which is placed by no one.
      int node = 1 + (int)(next() % (N - 1));
      int64_t row = ent_entity_row(w, picks[m]);
      ent_Picks_Pick_node(w)[row] = ids[node];
      ent_Picks_Pick_by(w)[row] = 1;
      if (s >= warmup)
        reached += (double)below[node];
    }
    double start = now();
    ent_step(w);
    if (s >= warmup)
      elapsed += now() - start;
  }

  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < N; ++i) {
    uint32_t bits = (uint32_t)WORLD(i);
    hash = (hash ^ bits) * 1099511628211ull;
  }
  printf("ns_per_step=%.0f reached=%.1f checksum=%016llx\n", elapsed / steps,
         moves ? reached / steps : 0.0, (unsigned long long)hash);
  return 0;
}
