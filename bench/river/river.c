// A river network (see examples/river.ent): N nodes in a tree, rain on
// each, and every node's flow the rain on all that is upstream of it,
// gathered from the leaves down. Built once per variant and size (N is a
// compile-time constant, since the ent-lang world's capacities are):
//
//   VARIANT 0  c-order   the nodes stay where they are, and a list of
//                        those with a parent, parents before children, is
//                        walked from its end; each adds its flow into its
//                        parent's, which it finds by its own number
//   VARIANT 3  c-pairs   what the generated code does, by hand: the same,
//                        with each node's parent next to it in the list
//   VARIANT 1  c-sorted  the nodes themselves stored in that order, so the
//                        walk reads its own flows one after another
//   VARIANT 2  ent       the ent-lang program compiled into the binary
//                        (as it is, or with its tree `sorted`, which is
//                        c-sorted's storage). With -DTWO the nodes are of
//                        two shapes, half of them in a second archetype
//                        `Pool` that the program is given, picked by the
//                        node's number: a tree across archetypes.
//
// The shape of the tree is an argument:
//   bushy     node i flows into a node picked from all before it (depth
//             about log N; a parent is anywhere)
//   deep      node i flows into one of the 8 before it (depth about N / 5;
//             a parent is next door)
//   shuffled  bushy, with the nodes in a random order (nothing is near)
//
// All variants add the flows into a node in the same order, so they agree
// to the bit; the checksum is a hash of every flow's bits. Compile with
// -ffp-contract=off.
//
// With a fourth argument the ent variants connect one node to the node it
// already flows into before every step, which changes nothing in the sums
// but is a change to the tree: what one costs. `resort` has the host
// connect it, which the next step takes in by going through every edge;
// `divert` has the program connect it (its schedule `divert`), which an
// unsorted tree takes in on the spot. The C variants take no notice.
//
// A sorted tree across archetypes adds the flows into a node in another
// order (by archetype, then row), so its checksum is its own; `total`, the
// sum of all flows, says that it is the same river.
//
// Usage: river STEPS WARMUP SHAPE [resort|divert]
// Prints: ns_per_step=... depth=... checksum=... total=...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if VARIANT == 2
#include "river_world.h"
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

// The tree: the node each flows into (-1 for none), and the rain on each.
static int32_t *parent;
static float *rain;

#if VARIANT != 2
// The nodes with a parent, parents before children, as the ent-lang sort
// lists them: the children of the nodes without a parent, by their own
// number, then the children of each node listed, by theirs.
static int32_t *order, *order_parent;
static int32_t listed;
static float *flow;
#if VARIANT == 1
// Where each node is stored (those without a parent first, then the
// list), and the tree in those places.
static int32_t *place, *parent_at;
static float *rain_at;
static int32_t roots;
#endif

static void list_nodes(void) {
  int32_t *first = calloc(N + 1, sizeof(int32_t));
  int32_t *children = malloc(sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0)
      ++first[parent[i] + 1];
  for (int i = 0; i < N; ++i)
    first[i + 1] += first[i];
  int32_t *cursor = malloc(sizeof(int32_t) * N);
  memcpy(cursor, first, sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0)
      children[cursor[parent[i]]++] = i;
  order = malloc(sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0 && parent[parent[i]] < 0)
      order[listed++] = i;
  for (int head = 0; head < listed; ++head)
    for (int c = first[order[head]]; c < first[order[head] + 1]; ++c)
      order[listed++] = children[c];
  order_parent = malloc(sizeof(int32_t) * N);
  for (int k = 0; k < listed; ++k)
    order_parent[k] = parent[order[k]];
  free(first);
  free(children);
  free(cursor);
}

__attribute__((noinline)) static void step(float wet) {
#if VARIANT == 0
  for (int i = 0; i < N; ++i)
    flow[i] = rain[i] * wet;
  for (int k = listed - 1; k >= 0; --k) {
    int32_t i = order[k];
    flow[parent[i]] += flow[i];
  }
#elif VARIANT == 3
  for (int i = 0; i < N; ++i)
    flow[i] = rain[i] * wet;
  for (int k = listed - 1; k >= 0; --k)
    flow[order_parent[k]] += flow[order[k]];
#else
  for (int i = 0; i < N; ++i)
    flow[i] = rain_at[i] * wet;
  for (int i = N - 1; i >= roots; --i)
    flow[parent_at[i]] += flow[i];
#endif
}
#endif

int main(int argc, char **argv) {
  int steps = argc > 1 ? atoi(argv[1]) : 100;
  int warmup = argc > 2 ? atoi(argv[2]) : 10;
  const char *shape = argc > 3 ? argv[3] : "bushy";
  int resort = argc > 4 && strcmp(argv[4], "divert") != 0;
  int divert = argc > 4 && strcmp(argv[4], "divert") == 0;
  (void)resort;
  (void)divert;

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
    for (int i = 0; i < N; ++i)
      moved[to[i]] = parent[i] < 0 ? -1 : to[parent[i]];
    free(parent);
    free(to);
    parent = moved;
  }
  int depth = 0;
  {
    int32_t *depths = calloc(N, sizeof(int32_t));
    // A node's depth, once its parent's is known: by walking up.
    for (int i = 0; i < N; ++i) {
      int d = 0, at = i;
      while (parent[at] >= 0 && depths[at] == 0) {
        at = parent[at];
        ++d;
      }
      d += depths[at];
      for (int back = i, left = d; parent[back] >= 0 && depths[back] == 0;
           back = parent[back], --left)
        depths[back] = left;
      if (d > depth)
        depth = d;
    }
    free(depths);
  }

#if VARIANT == 2
  ent_world *w = ent_world_create();
  if (!w) {
    fprintf(stderr, "cannot allocate the world\n");
    return 1;
  }
  ent_entity *ids = malloc(sizeof(ent_entity) * N);
#ifdef TWO
  // Which nodes are pools: by a hash of the number, not by the generator
  // the tree is made with.
  #define POOL(i) (((uint32_t)(i) * 2654435761u >> 16) & 1)
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = POOL(i) ? ent_Pool_spawn(w) : ent_Cell_spawn(w);
    int64_t row = ent_entity_row(w, id);
    if (POOL(i)) {
      ent_Pool_Node_rain(w)[row] = rain[i];
      ent_Pool_Node_flow(w)[row] = 0.0f;
      ent_Pool_Still_level(w)[row] = 0.0f;
    } else {
      ent_Cell_Node_rain(w)[row] = rain[i];
      ent_Cell_Node_flow(w)[row] = 0.0f;
    }
  }
#else
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = ent_Cell_spawn(w);
    int64_t row = ent_entity_row(w, id);
    ent_Cell_Node_rain(w)[row] = rain[i];
    ent_Cell_Node_flow(w)[row] = 0.0f;
  }
#endif
  for (int i = 0; i < N; ++i)
    if (parent[i] >= 0 && !ent_Flows_connect(w, ids[i], ids[parent[i]]))
      return 1;
#define STEP(wet)                                                            \
  ((void)(resort && ent_Flows_connect(w, ids[N - 1], ids[parent[N - 1]])),   \
   (void)(divert && (ent_divert(w, ids[N - 1], ids[parent[N - 1]]), 1)),     \
   ent_step(w, wet))
#ifdef TWO
#define FLOW(i)                                                              \
  ((POOL(i) ? ent_Pool_Node_flow(w)                                          \
            : ent_Cell_Node_flow(w))[ent_entity_row(w, ids[i])])
#else
#define FLOW(i) (ent_Cell_Node_flow(w)[ent_entity_row(w, ids[i])])
#endif
#else
  list_nodes();
  flow = calloc(N, sizeof(float));
#if VARIANT == 1
  place = malloc(sizeof(int32_t) * N);
  parent_at = malloc(sizeof(int32_t) * N);
  rain_at = malloc(sizeof(float) * N);
  for (int i = 0; i < N; ++i)
    if (parent[i] < 0)
      place[i] = roots++;
  for (int k = 0; k < listed; ++k)
    place[order[k]] = roots + k;
  for (int i = 0; i < N; ++i) {
    parent_at[place[i]] = parent[i] < 0 ? -1 : place[parent[i]];
    rain_at[place[i]] = rain[i];
  }
#define FLOW(i) (flow[place[i]])
#else
#define FLOW(i) (flow[i])
#endif
#define STEP(wet) step(wet)
#endif

  // Warm up (the first ent step also sorts the edges and lists the nodes).
  for (int s = 0; s < warmup; ++s)
    STEP(1.0f + (float)(s % 7) * 0.125f);
  double start = now();
  for (int s = 0; s < steps; ++s)
    STEP(1.0f + (float)(s % 7) * 0.125f);
  double elapsed = now() - start;

  uint64_t hash = 1469598103934665603ull;
  double total = 0.0;
  for (int i = 0; i < N; ++i) {
    float f = FLOW(i);
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    hash = (hash ^ bits) * 1099511628211ull;
    total += f;
  }
  printf("ns_per_step=%.0f depth=%d checksum=%016llx total=%.9g\n",
         elapsed / steps, depth, (unsigned long long)hash, total);
  return 0;
}
