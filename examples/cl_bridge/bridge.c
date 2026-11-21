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
//
// Built for a GPU (tools/ent build --gpu --shared), the systems of a step
// run there, and between steps only what the host touches comes back to
// it: the electrodes' counts after a step, a stim's cell around its
// write. Such a process may have one dish (the GPU keeps one world):
// dish_create gives NULL for a second.
#include "ent_world.h"

#ifdef ENT_GPU
#define BACK(at, size) ent_gpu_range_to_host((at), (size))
#define WROTE(at, size) ent_gpu_range_wrote((at), (size))
#else
#define BACK(at, size) ((void)0)
#define WROTE(at, size) ((void)0)
#endif

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
  float adapt_inc, tau_adapt, reach_cut;
  float depress, tau_recover, hear_reach, tau_syn;
  float home_rate, tau_home, home_gain;
  float axon, fast, conduct;
  int32_t modulated;
  float tau_elig, tau_reward, reward_gain;
  int32_t reward_channel, punish_channel;
  int32_t hard;
} dish_params;

// By name, for a host that does not know the struct's layout (a web
// page's JavaScript, game.c): gives 0 where there is no such parameter.
#include <stddef.h>
#define FIELD(name, kind) {#name, offsetof(dish_params, name), kind}
static const struct { const char *name; size_t at; char kind; } FIELDS[] = {
    FIELD(seed, 'l'), FIELD(neurons, 'i'), FIELD(inhibitory, 'f'), FIELD(tau, 'f'),
    FIELD(thr, 'f'), FIELD(refractory, 'f'), FIELD(density, 'f'), FIELD(reach, 'f'),
    FIELD(w_exc, 'f'), FIELD(w_inh, 'f'), FIELD(w_max, 'f'), FIELD(stdp, 'i'),
    FIELD(a_plus, 'f'), FIELD(a_minus, 'f'), FIELD(tau_plus, 'f'), FIELD(tau_minus, 'f'),
    FIELD(noise_rate, 'f'), FIELD(noise_amp, 'f'), FIELD(stim_gain, 'f'),
    FIELD(stim_reach, 'f'), FIELD(adapt_inc, 'f'), FIELD(tau_adapt, 'f'),
    FIELD(reach_cut, 'f'), FIELD(depress, 'f'), FIELD(tau_recover, 'f'),
    FIELD(hear_reach, 'f'), FIELD(tau_syn, 'f'), FIELD(home_rate, 'f'),
    FIELD(tau_home, 'f'), FIELD(home_gain, 'f'), FIELD(axon, 'f'), FIELD(fast, 'f'),
    FIELD(conduct, 'f'), FIELD(modulated, 'i'), FIELD(tau_elig, 'f'),
    FIELD(tau_reward, 'f'), FIELD(reward_gain, 'f'), FIELD(reward_channel, 'i'),
    FIELD(punish_channel, 'i'), FIELD(hard, 'i'),
};

int32_t dish_params_size(void) { return (int32_t)sizeof(dish_params); }

int32_t dish_params_set(dish_params *p, const char *name, double value) {
  for (size_t i = 0; i < sizeof FIELDS / sizeof *FIELDS; ++i)
    if (strcmp(FIELDS[i].name, name) == 0) {
      char *at = (char *)p + FIELDS[i].at;
      if (FIELDS[i].kind == 'l')
        *(int64_t *)at = (int64_t)value;
      else if (FIELDS[i].kind == 'i')
        *(int32_t *)at = (int32_t)value;
      else
        *(float *)at = (float)value;
      return 1;
    }
  return 0;
}

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
  p->adapt_inc = *ent_Dish_adapt_inc(w);
  p->tau_adapt = *ent_Dish_tau_adapt(w);
  p->reach_cut = *ent_Dish_reach_cut(w);
  p->depress = *ent_Dish_depress(w);
  p->tau_recover = *ent_Dish_tau_recover(w);
  p->hear_reach = *ent_Dish_hear_reach(w);
  p->tau_syn = *ent_Dish_tau_syn(w);
  p->home_rate = *ent_Dish_home_rate(w);
  p->tau_home = *ent_Dish_tau_home(w);
  p->home_gain = *ent_Dish_home_gain(w);
  p->axon = *ent_Dish_axon(w);
  p->fast = *ent_Dish_fast(w);
  p->conduct = *ent_Dish_conduct(w);
  p->modulated = *ent_Dish_modulated(w);
  p->tau_elig = *ent_Dish_tau_elig(w);
  p->tau_reward = *ent_Dish_tau_reward(w);
  p->reward_gain = *ent_Dish_reward_gain(w);
  p->reward_channel = *ent_Dish_reward_channel(w);
  p->punish_channel = *ent_Dish_punish_channel(w);
  p->hard = *ent_Dish_hard(w);
  ent_world_destroy(w);
}

// The largest number of neurons a dish may have (network.ent's capacity
// of Cell).
int32_t dish_most_neurons(void) { return 65536; }

dish *dish_create(const dish_params *p) {
  if (p->neurons < 1 || p->neurons > dish_most_neurons())
    return NULL;
#ifdef ENT_GPU
  static int made;
  if (made)
    return NULL;
  made = 1;
#endif
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
  *ent_Dish_adapt_inc(w) = p->adapt_inc;
  *ent_Dish_tau_adapt(w) = p->tau_adapt;
  *ent_Dish_reach_cut(w) = p->reach_cut;
  *ent_Dish_depress(w) = p->depress;
  *ent_Dish_tau_recover(w) = p->tau_recover;
  *ent_Dish_hear_reach(w) = p->hear_reach;
  *ent_Dish_tau_syn(w) = p->tau_syn;
  *ent_Dish_home_rate(w) = p->home_rate;
  *ent_Dish_tau_home(w) = p->tau_home;
  *ent_Dish_home_gain(w) = p->home_gain;
  *ent_Dish_axon(w) = p->axon;
  *ent_Dish_fast(w) = p->fast;
  *ent_Dish_conduct(w) = p->conduct;
  *ent_Dish_modulated(w) = p->modulated != 0;
  *ent_Dish_tau_elig(w) = p->tau_elig;
  *ent_Dish_tau_reward(w) = p->tau_reward;
  *ent_Dish_reward_gain(w) = p->reward_gain;
  *ent_Dish_reward_channel(w) = p->reward_channel;
  *ent_Dish_punish_channel(w) = p->punish_channel;
  *ent_Dish_hard(w) = p->hard != 0;
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
  float *stim = &ent_Pad_Electrode_stim(d->world)[channel];
  BACK(stim, sizeof *stim);
  *stim += amount;
  WROTE(stim, sizeof *stim);
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
    int32_t *tick = ent_Dish_tick(w);
    BACK(tick, sizeof *tick);
    int32_t step = *tick;
    ent_step(w);
    const int32_t *count = ent_Pad_Electrode_count(w);
    BACK(count, 64 * sizeof *count);
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

// Turns learning (STDP) on or off from the next step on.
void dish_set_stdp(dish *d, int32_t on) {
  bool *stdp = ent_Dish_stdp(d->world);
  BACK(stdp, sizeof *stdp);
  *stdp = on != 0;
  WROTE(stdp, sizeof *stdp);
}

// Sets how fast the neurons scale their excitatory synapses (home_gain;
// 0: not at all) from the next step on.
void dish_set_home_gain(dish *d, float gain) {
  float *home_gain = ent_Dish_home_gain(d->world);
  BACK(home_gain, sizeof *home_gain);
  *home_gain = gain;
  WROTE(home_gain, sizeof *home_gain);
}

// Sets how far a reward or punish pulse moves the weights (reward_gain,
// the learning rate of the modulated rule; 0: not at all) from the next
// step on.
void dish_set_reward_gain(dish *d, float gain) {
  float *reward_gain = ent_Dish_reward_gain(d->world);
  BACK(reward_gain, sizeof *reward_gain);
  *reward_gain = gain;
  WROTE(reward_gain, sizeof *reward_gain);
}

// The steps run so far.
int32_t dish_tick(dish *d) {
  int32_t *tick = ent_Dish_tick(d->world);
  BACK(tick, sizeof *tick);
  return *tick;
}

// All spikes so far.
int64_t dish_spike_total(dish *d) {
  int64_t *spikes = ent_Dish_spikes(d->world);
  BACK(spikes, sizeof *spikes);
  return *spikes;
}

// A number of all potentials and weights (network.ent's checksum).
uint64_t dish_checksum(dish *d) {
  ent_checksum(d->world);
  return *ent_Dish_check(d->world);
}

// The mean excitatory weight (runs the checksum).
float dish_mean_weight(dish *d) {
  ent_checksum(d->world);
  return *ent_Dish_w_mean(d->world);
}

// The excitatory weights by electrode pair: w[a * 64 + b] the sum of the
// weights of the plastic synapses from the neurons of channel a to those
// of channel b, n[a * 64 + b] how many there are (network.ent's `paths`).
void dish_paths(dish *d, float *w, float *n) {
  ent_paths(d->world);
  const int32_t *index = ent_Link_Path_index(d->world);
  const float *pw = ent_Link_Path_w(d->world);
  const float *pn = ent_Link_Path_n(d->world);
  BACK(index, 4096 * sizeof *index);
  BACK(pw, 4096 * sizeof *pw);
  BACK(pn, 4096 * sizeof *pn);
  for (int i = 0; i < 4096; i++) {
    w[index[i]] = pw[i];
    n[index[i]] = pn[i];
  }
}
