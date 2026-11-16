// The dish of network.ent as a shared library with a flat C interface,
// for a host in any language (ent_dish.py loads it with ctypes):
//
//   tools/ent build --shared examples/cl_bridge/network.ent
//       examples/cl_bridge/bridge.c -o libdish.so      (one line)
//
//   dish_params p;  dish_defaults(&p);  p.seed = 7;
//   dish *d = dish_create(&p);
//   dish_stimulate(d, 12, 1.0f);       // at the next step
//   dish_step(d, 25);                  // 25 ms of the dish
//   int n = dish_spikes(d, channels, steps, max);
//   uint64_t sum = dish_checksum(d);
//   dish_destroy(d);
//
// Every function takes the dish it is about: a process may have several
// (two of a seed, with STDP and without, say). A step is a millisecond.
// Each spike is given as its channel and the step it happened in, those of
// a step in the order of their channels; dish_spikes hands them out in the
// order they happened and forgets those it handed out.
#include "ent_world.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t seed;
  int32_t neurons;
  float inhibitory, tau, thr, refractory, density, reach;
  float w_exc, w_inh, w_max;
  int32_t stdp;
  float a_plus, a_minus, tau_plus, tau_minus;
  float noise_rate, noise_amp, stim_gain, stim_reach;
} dish_params;

typedef struct {
  ent_world *world;
  int32_t *channel, *step;  // the spikes not yet handed out
  int64_t first, count, room;
} dish;

// The parameters network.ent's world starts with.
void dish_defaults(dish_params *p) {
  ent_world *w = ent_world_create();
  if (!w)
    return;
  ent_world_init(w);
  p->seed = *ent_Dish_seed(w);
  p->neurons = *ent_Dish_neurons(w);
  p->inhibitory = *ent_Dish_inhibitory(w);
  p->tau = *ent_Dish_tau(w);
  p->thr = *ent_Dish_thr(w);
  p->refractory = *ent_Dish_refractory(w);
  p->density = *ent_Dish_density(w);
  p->reach = *ent_Dish_reach(w);
  p->w_exc = *ent_Dish_w_exc(w);
  p->w_inh = *ent_Dish_w_inh(w);
  p->w_max = *ent_Dish_w_max(w);
  p->stdp = *ent_Dish_stdp(w);
  p->a_plus = *ent_Dish_a_plus(w);
  p->a_minus = *ent_Dish_a_minus(w);
  p->tau_plus = *ent_Dish_tau_plus(w);
  p->tau_minus = *ent_Dish_tau_minus(w);
  p->noise_rate = *ent_Dish_noise_rate(w);
  p->noise_amp = *ent_Dish_noise_amp(w);
  p->stim_gain = *ent_Dish_stim_gain(w);
  p->stim_reach = *ent_Dish_stim_reach(w);
  ent_world_destroy(w);
}

// The largest number of neurons a dish may have (network.ent's capacity
// of Cell).
int32_t dish_most_neurons(void) { return 4096; }

dish *dish_create(const dish_params *p) {
  if (p->neurons < 1 || p->neurons > dish_most_neurons())
    return NULL;
  dish *d = calloc(1, sizeof *d);
  if (!d)
    return NULL;
  d->world = ent_world_create();
  if (!d->world) {
    free(d);
    return NULL;
  }
  ent_world *w = d->world;
  ent_world_init(w);
  *ent_Dish_seed(w) = p->seed;
  *ent_Dish_neurons(w) = p->neurons;
  *ent_Dish_inhibitory(w) = p->inhibitory;
  *ent_Dish_tau(w) = p->tau;
  *ent_Dish_thr(w) = p->thr;
  *ent_Dish_refractory(w) = p->refractory;
  *ent_Dish_density(w) = p->density;
  *ent_Dish_reach(w) = p->reach;
  *ent_Dish_w_exc(w) = p->w_exc;
  *ent_Dish_w_inh(w) = p->w_inh;
  *ent_Dish_w_max(w) = p->w_max;
  *ent_Dish_stdp(w) = p->stdp != 0;
  *ent_Dish_a_plus(w) = p->a_plus;
  *ent_Dish_a_minus(w) = p->a_minus;
  *ent_Dish_tau_plus(w) = p->tau_plus;
  *ent_Dish_tau_minus(w) = p->tau_minus;
  *ent_Dish_noise_rate(w) = p->noise_rate;
  *ent_Dish_noise_amp(w) = p->noise_amp;
  *ent_Dish_stim_gain(w) = p->stim_gain;
  *ent_Dish_stim_reach(w) = p->stim_reach;
  ent_create(w);
  return d;
}

void dish_destroy(dish *d) {
  if (!d)
    return;
  ent_world_destroy(d->world);
  free(d->channel);
  free(d->step);
  free(d);
}

// Adds to a channel's stim, which its neurons take in at the next step.
// A channel outside 0..63 is no channel.
void dish_stimulate(dish *d, int32_t channel, float amount) {
  if (channel < 0 || channel >= 64)
    return;
  // (The electrodes were spawned first, by channel: electrode c is row c.)
  ent_Pad_Electrode_stim(d->world)[channel] += amount;
}

static int keep(dish *d, int32_t channel, int32_t step) {
  if (d->first + d->count == d->room) {
    if (d->first > 0) {
      memmove(d->channel, d->channel + d->first, d->count * sizeof *d->channel);
      memmove(d->step, d->step + d->first, d->count * sizeof *d->step);
      d->first = 0;
    }
    if (d->count == d->room) {
      int64_t room = d->room ? 2 * d->room : 4096;
      int32_t *c = realloc(d->channel, room * sizeof *c);
      if (!c)
        return 0;
      d->channel = c;
      int32_t *s = realloc(d->step, room * sizeof *s);
      if (!s)
        return 0;
      d->step = s;
      d->room = room;
    }
  }
  d->channel[d->first + d->count] = channel;
  d->step[d->first + d->count] = step;
  d->count++;
  return 1;
}

// Runs n steps; gives how many spikes they had (0 or more), or -1 where
// there was no room to keep them.
int64_t dish_step(dish *d, int32_t n) {
  ent_world *w = d->world;
  int64_t total = 0;
  for (int32_t i = 0; i < n; ++i) {
    int32_t step = *ent_Dish_tick(w);
    ent_step(w);
    const int32_t *count = ent_Pad_Electrode_count(w);
    for (int32_t c = 0; c < 64; ++c)
      for (int32_t k = 0; k < count[c]; ++k) {
        if (!keep(d, c, step))
          return -1;
        total++;
      }
  }
  return total;
}

// Hands out up to max of the spikes not yet handed out, the earliest
// first: each one's channel and step. Gives how many.
int64_t dish_spikes(dish *d, int32_t *channel, int32_t *step, int64_t max) {
  int64_t n = d->count < max ? d->count : max;
  memcpy(channel, d->channel + d->first, n * sizeof *channel);
  memcpy(step, d->step + d->first, n * sizeof *step);
  d->first += n;
  d->count -= n;
  if (d->count == 0)
    d->first = 0;
  return n;
}

// The steps run so far.
int32_t dish_tick(dish *d) { return *ent_Dish_tick(d->world); }

// All spikes so far.
int64_t dish_spike_total(dish *d) { return *ent_Dish_spikes(d->world); }

// A number of all potentials and weights (network.ent's checksum).
uint64_t dish_checksum(dish *d) {
  ent_checksum(d->world);
  return *ent_Dish_check(d->world);
}
