// Host for snn.ent and snn_pull.ent: spawns N neurons with a bias each,
// connects K random synapses per neuron, runs STEPS steps, and runs the
// same network as plain C next to it (compressed rows by source, pushing
// in source order), checking that every potential agrees to the bit.
// Compile with -ffp-contract=off so that C keeps v * decay + input + bias
// as two roundings, like the generated code.

#include "snn_world.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { N = 1024, K = 32, STEPS = 100 };

static uint32_t state = 12345;
static uint32_t next(void) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}
static float uniform(float low, float high) {
  return low + (high - low) * (float)next() / (float)(1u << 24);
}

static float v[N], input[N], bias[N];
static int fired[N];
static int offsets[N + 1], targets[N * K];
static float weights[N * K];

int main(void) {
  const float decay = 0.9f, threshold = 1.0f;
  ent_world *w = ent_world_create();
  ent_entity ids[N];
  for (int i = 0; i < N; ++i) {
    ids[i] = ent_Cell_spawn(w);
    int64_t row = ent_entity_row(w, ids[i]);
    bias[i] = uniform(0.02f, 0.15f);
    ent_Cell_Neuron_v(w)[row] = 0.0f;
    ent_Cell_Neuron_input(w)[row] = 0.0f;
    ent_Cell_Neuron_bias(w)[row] = bias[i];
    ent_Cell_Neuron_fired(w)[row] = 0.0f;
  }
  for (int i = 0; i < N; ++i) {
    offsets[i] = i * K;
    for (int k = 0; k < K; ++k) {
      int target = (int)(next() % N);
      float weight = uniform(-0.15f, 0.12f);
      targets[i * K + k] = target;
      weights[i * K + k] = weight;
      if (!ent_Synapse_connect(w, ids[i], ids[target], weight)) {
        printf("relation full\n");
        return 1;
      }
    }
  }
  offsets[N] = N * K;

  long long spikes = 0;
  for (int step = 0; step < STEPS; ++step) {
    ent_step(w, decay, threshold);
    // The same step in C.
    for (int i = 0; i < N; ++i) {
      float x = v[i] * decay + input[i] + bias[i];
      input[i] = 0.0f;
      fired[i] = x >= threshold;
      v[i] = fired[i] ? 0.0f : x;
      spikes += fired[i];
    }
    for (int i = 0; i < N; ++i)
      if (fired[i])
        for (int e = offsets[i]; e < offsets[i + 1]; ++e)
          input[targets[e]] += weights[e];
  }

  int same = 1;
  double sum = 0;
  for (int i = 0; i < N; ++i) {
    int64_t row = ent_entity_row(w, ids[i]);
    float ev = ent_Cell_Neuron_v(w)[row], ei = ent_Cell_Neuron_input(w)[row];
    same &= memcmp(&ev, &v[i], sizeof ev) == 0 &&
            memcmp(&ei, &input[i], sizeof ei) == 0;
    sum += ev;
  }
  printf("neurons %d synapses %lld steps %d\n", N, (long long)ent_Synapse_count(w),
         STEPS);
  printf("spikes %lld (C %lld) sum of v %.6f\n",
         (long long)*ent_Stats_spikes(w), spikes, sum);
  printf("matches C: %s\n", same ? "yes" : "no");
  ent_world_destroy(w);
  return 0;
}
