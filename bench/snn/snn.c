// A spiking network of leaky integrate-and-fire neurons (see
// examples/snn.ent): N neurons with K random synapses each, run for some
// steps. Built once per variant and network size (N and K are compile-time
// constants, since the ent-lang world's capacities are):
//
//   VARIANT 0  c-push      compressed rows by source; firing neurons add
//                          their weights into their targets' input
//   VARIANT 1  c-pull      compressed rows by target; every neuron sums the
//                          weights from sources that fired
//   VARIANT 2  c-pull-par  the same, neurons in parallel (OpenMP)
//   VARIANT 3  ent         the ent-lang program compiled into the binary
//                          (push or pull, sequential or parallel)
//
// Diagnostic variants, each adding one thing the generated pull does to
// c-pull, to tell where its time goes:
//   VARIANT 4  c-pull-index               sources and weights stay in source
//                                         order and are read through the
//                                         positions of the edges by target
//   VARIANT 5  c-pull-index-store         ... and the input is loaded and
//                                         stored on every edge
//   VARIANT 6  c-pull-index-store-locate  ... and every source id is checked
//                                         like an ent-lang lookup (archetype
//                                         and row bound) before the load
//   VARIANT 8  c-push-buffer              c-push in the generated code's two
//                                         passes: every row writes a flag,
//                                         firing rows copy their edges'
//                                         targets and values into buffers;
//                                         then every row's flag is read and
//                                         the buffered values are added in
//   VARIANT 7  c-pull-locate              c-pull with only the lookup's
//                                         checks (the generated pull since
//                                         the edges are sorted by target and
//                                         the sum is kept in a register)
//
// All variants add the weights into a neuron's input in the order of the
// source neurons, so they agree to the bit; the checksum is a hash of every
// potential's bits. Compile with -ffp-contract=off.
//
// Usage: snn STEPS WARMUP BIAS_HIGH
// Prints: ns_per_step=... spikes_per_step=... checksum=...

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if VARIANT == 3
#include "snn_world.h"
#endif

#ifndef N
#error "define N and K"
#endif

static uint32_t state = 12345;
static uint32_t next(void) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}
static float uniform(float low, float high) {
  return low + (high - low) * (float)next() / (float)(1u << 24);
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static const float decay = 0.9f, threshold = 1.0f;
static float *bias;
static int32_t *targets, *sources;
static float *weights;
// For VARIANT 6: the bits of a row, and an entity count to check against
// (read from memory, as the generated code does).
#define ROW_BITS 31
static volatile int64_t count_storage = N;
static int64_t count;

#if VARIANT != 3
static float *v, *input, *fired;
static int32_t *in_offsets, *in_sources, *in_edges;
static float *in_weights;
// For VARIANT 8: per row, -1 if it sent nothing; per edge, a target and a
// value.
static int32_t *ran, *sent_to;
static float *sent;
static long long spikes;

static void integrate(void) {
  for (int i = 0; i < N; ++i) {
    float x = v[i] * decay + input[i] + bias[i];
    input[i] = 0.0f;
    if (x >= threshold) {
      v[i] = 0.0f;
      fired[i] = 1.0f;
      ++spikes;
    } else {
      v[i] = x;
      fired[i] = 0.0f;
    }
  }
}

static void step(void) {
  integrate();
#if VARIANT == 0
  for (int i = 0; i < N; ++i)
    if (fired[i] != 0.0f)
      for (int e = i * K; e < (i + 1) * K; ++e)
        input[targets[e]] += weights[e];
#elif VARIANT == 8
  for (int i = 0; i < N; ++i) {
    ran[i] = -1;
    if (fired[i] != 0.0f) {
      ran[i] = 0;
      for (int e = i * K; e < (i + 1) * K; ++e) {
        sent_to[e] = targets[e];
        sent[e] = weights[e];
      }
    }
  }
  for (int i = 0; i < N; ++i)
    if (ran[i] != -1)
      for (int e = i * K; e < (i + 1) * K; ++e)
        if (sent_to[e] != -1)
          input[sent_to[e]] += sent[e];
#elif VARIANT == 7
  // The sum in a local, as the generated code carries it (with the checks,
  // clang would otherwise store it on every edge).
  for (int i = 0; i < N; ++i) {
    float sum = input[i];
    for (int e = in_offsets[i]; e < in_offsets[i + 1]; ++e) {
      int32_t source = in_sources[e];
      // Row ids: archetype 0 in the high bits, the row below a count.
      if ((uint32_t)source >> ROW_BITS != 0 || source >= count)
        continue;
      sum += in_weights[e] * fired[source];
    }
    input[i] = sum;
  }
#elif VARIANT <= 2
#if VARIANT == 2
#pragma omp parallel for schedule(static)
#endif
  for (int i = 0; i < N; ++i)
    for (int e = in_offsets[i]; e < in_offsets[i + 1]; ++e)
      input[i] += in_weights[e] * fired[in_sources[e]];
#else
  // Sources are e / K in the table by source; stored like the generated
  // code, as ids, here equal to rows.
  for (int i = 0; i < N; ++i) {
    for (int e = in_offsets[i]; e < in_offsets[i + 1]; ++e) {
      int32_t k = in_edges[e];
      int32_t source = sources[k];
#if VARIANT == 6
      // Row ids: archetype 0 in the high bits, the row below a count.
      if ((uint32_t)source >> ROW_BITS != 0 || source >= count)
        continue;
#endif
      input[i] += weights[k] * fired[source];
#if VARIANT >= 5
      // Forbid keeping the sum in a register, like the generated code
      // (whose arena views LLVM cannot tell apart).
      __asm__ volatile("" ::: "memory");
#endif
    }
  }
#endif
}
#endif

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s STEPS WARMUP BIAS_HIGH\n", argv[0]);
    return 2;
  }
  int steps = atoi(argv[1]), warmup = atoi(argv[2]);
  float biasHigh = (float)atof(argv[3]);
  bias = malloc(sizeof(float) * N);
  targets = malloc(sizeof(int32_t) * N * K);
  weights = malloc(sizeof(float) * N * K);
  // Weights scale with 1/K, so the input a neuron gets has about the same
  // spread for every K.
  float scale = 32.0f / K;
  for (int i = 0; i < N; ++i)
    bias[i] = uniform(0.02f, biasHigh);
  for (int e = 0; e < N * K; ++e) {
    targets[e] = (int32_t)(next() % N);
    weights[e] = uniform(-0.15f, 0.12f) * scale;
  }

#if VARIANT == 3
  ent_world *w = ent_world_create();
  if (!w) {
    fprintf(stderr, "cannot allocate the world\n");
    return 1;
  }
  ent_entity *ids = malloc(sizeof(ent_entity) * N);
  for (int i = 0; i < N; ++i) {
    ent_entity id = ids[i] = ent_Cell_spawn(w);
    int64_t row = ent_entity_row(w, id);
    ent_Cell_Neuron_v(w)[row] = 0.0f;
    ent_Cell_Neuron_input(w)[row] = 0.0f;
    ent_Cell_Neuron_bias(w)[row] = bias[i];
    ent_Cell_Neuron_fired(w)[row] = 0.0f;
    ent_Cell_Spiked_present(w)[row] = 0;
  }
  for (int i = 0; i < N; ++i)
    for (int e = i * K; e < (i + 1) * K; ++e)
      if (!ent_Synapse_connect(w, ids[i], ids[targets[e]], weights[e]))
        return 1;
#define STEP() ent_step(w, decay, threshold)
#define SPIKES() (*ent_Stats_spikes(w))
#define POTENTIAL(i) (ent_Cell_Neuron_v(w)[ent_entity_row(w, ids[i])])
#else
  v = calloc(N, sizeof(float));
  input = calloc(N, sizeof(float));
  fired = calloc(N, sizeof(float));
  in_offsets = calloc(N + 1, sizeof(int32_t));
  in_sources = malloc(sizeof(int32_t) * N * K);
  in_weights = malloc(sizeof(float) * N * K);
  in_edges = malloc(sizeof(int32_t) * N * K);
  ran = malloc(sizeof(int32_t) * N);
  sent_to = malloc(sizeof(int32_t) * N * K);
  sent = malloc(sizeof(float) * N * K);
  sources = malloc(sizeof(int32_t) * N * K);
  for (int e = 0; e < N * K; ++e)
    sources[e] = e / K;
  count = count_storage;
  for (int e = 0; e < N * K; ++e)
    ++in_offsets[targets[e] + 1];
  for (int i = 0; i < N; ++i)
    in_offsets[i + 1] += in_offsets[i];
  int32_t *cursor = malloc(sizeof(int32_t) * N);
  memcpy(cursor, in_offsets, sizeof(int32_t) * N);
  for (int e = 0; e < N * K; ++e) {
    int32_t at = cursor[targets[e]]++;
    in_sources[at] = e / K;
    in_weights[at] = weights[e];
    in_edges[at] = e;
  }
  free(cursor);
#define STEP() step()
#define SPIKES() spikes
#define POTENTIAL(i) (v[i])
#endif

  // Warm up (the first ent step also sorts the edges).
  for (int s = 0; s < warmup; ++s)
    STEP();
  long long before = SPIKES();
  double start = now();
  for (int s = 0; s < steps; ++s)
    STEP();
  double elapsed = now() - start;
  long long during = SPIKES() - before;

  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < N; ++i) {
    float p = POTENTIAL(i);
    uint32_t bits;
    memcpy(&bits, &p, sizeof bits);
    hash = (hash ^ bits) * 1099511628211ull;
  }
  printf("ns_per_step=%.0f spikes_per_step=%.1f checksum=%016llx\n",
         elapsed / steps, (double)during / steps, (unsigned long long)hash);
  return 0;
}
