// The dish through its C interface (bridge.c), with no Python:
//
//   1. the same seed and parameters twice give the same spikes (each
//      one's channel and step, in order) and the same checksum; another
//      seed other spikes;
//   2. a stim makes its channel fire: two dishes of one seed, one given a
//      stim on a channel every 100 ms, the other not, and the spikes on
//      that channel in the 20 ms after each stim counted in both.
//
//   tools/ent build --shared examples/cl_bridge/network.ent
//       examples/cl_bridge/bridge.c -o libdish.so      (one line)
//   cc -O2 dish_check.c -L. -ldish -Wl,-rpath,. -o dish_check && ./dish_check
//
// `dish_check one` runs a single dish (seed 1, a stim on channel 27 every
// 100 ms) and says its spikes and checksum: what a library built for a
// GPU, which has one dish a process, gives, to hold against the CPU's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int64_t seed;
  int32_t neurons;
  float inhibitory, tau, thr, refractory, density, reach;
  float w_exc, w_inh, w_max;
  int32_t stdp;
  float a_plus, a_minus, tau_plus, tau_minus;
  float noise_rate, noise_amp, stim_gain, stim_reach;
  float adapt_inc, tau_adapt, reach_cut;
} dish_params;
typedef struct dish dish;

void dish_defaults(dish_params *p);
dish *dish_create(const dish_params *p);
void dish_destroy(dish *d);
void dish_stimulate(dish *d, int32_t channel, float amount);
int64_t dish_step(dish *d, int32_t n);
int64_t dish_spikes(dish *d, int32_t *channel, int32_t *step, int64_t max);
uint64_t dish_checksum(dish *d);

enum { MOST = 1 << 20 };
static int32_t channels[MOST], steps[MOST];

// Runs a dish of this seed for `ms` steps; a hash of its spikes, in order,
// and how many there were.
static uint64_t run(int64_t seed, int ms, int64_t *count, uint64_t *sum) {
  dish_params p;
  dish_defaults(&p);
  p.seed = seed;
  dish *d = dish_create(&p);
  uint64_t h = 1469598103934665603ull;
  *count = 0;
  for (int t = 0; t < ms; t += 50) {
    dish_step(d, 50);
    int64_t n = dish_spikes(d, channels, steps, MOST);
    for (int64_t i = 0; i < n; ++i) {
      h = (h ^ (uint64_t)channels[i]) * 1099511628211ull;
      h = (h ^ (uint64_t)steps[i]) * 1099511628211ull;
    }
    *count += n;
  }
  *sum = dish_checksum(d);
  dish_destroy(d);
  return h;
}

static int one(void) {
  dish_params p;
  dish_defaults(&p);
  dish *d = dish_create(&p);
  uint64_t h = 1469598103934665603ull;
  int64_t count = 0, evoked = 0;
  for (int t = 0; t < 3000; ++t) {
    if (t >= 500 && (t - 500) % 100 == 0)
      dish_stimulate(d, 27, 1.0f);
    dish_step(d, 1);
    int64_t n = dish_spikes(d, channels, steps, MOST);
    for (int64_t i = 0; i < n; ++i) {
      h = (h ^ (uint64_t)channels[i]) * 1099511628211ull;
      h = (h ^ (uint64_t)steps[i]) * 1099511628211ull;
      evoked += channels[i] == 27 && t >= 500 && (t - 500) % 100 < 20;
    }
    count += n;
  }
  printf("one dish: %lld spikes, %lld on channel 27 in the 20 ms after a stim, "
         "hash %016llx, checksum %016llx\n", (long long)count, (long long)evoked,
         (unsigned long long)h, (unsigned long long)dish_checksum(d));
  dish_destroy(d);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && argv[1][0] == 'o')
    return one();
  int channel = argc > 1 ? atoi(argv[1]) : 27;
  float amount = argc > 2 ? (float)atof(argv[2]) : 1.0f;

  int64_t n1, n2, n3;
  uint64_t s1, s2, s3;
  uint64_t h1 = run(1, 2000, &n1, &s1);
  uint64_t h2 = run(1, 2000, &n2, &s2);
  uint64_t h3 = run(2, 2000, &n3, &s3);
  printf("seed 1: %lld spikes, hash %016llx, checksum %016llx\n",
         (long long)n1, (unsigned long long)h1, (unsigned long long)s1);
  printf("seed 1 again: %s\n", h1 == h2 && n1 == n2 && s1 == s2 ? "the same" : "DIFFERENT");
  printf("seed 2: %lld spikes, %s\n", (long long)n3,
         h3 != h1 && s3 != s1 ? "other spikes and checksum" : "THE SAME AS SEED 1");

  // The stim: every 100 ms, 20 times, from 500 ms on.
  dish_params p;
  dish_defaults(&p);
  p.seed = 1;
  dish *with = dish_create(&p), *without = dish_create(&p);
  int64_t evoked = 0, spontaneous = 0, other_with = 0, other_without = 0;
  for (int t = 0; t < 2500; ++t) {
    int stim = t >= 500 && (t - 500) % 100 == 0;
    if (stim)
      dish_stimulate(with, channel, amount);
    dish_step(with, 1);
    dish_step(without, 1);
    int64_t a = dish_spikes(with, channels, steps, MOST);
    int64_t inside = t >= 500 && (t - 500) % 100 < 20;
    for (int64_t i = 0; i < a; ++i)
      if (inside) {
        if (channels[i] == channel)
          evoked++;
        else
          other_with++;
      }
    int64_t b = dish_spikes(without, channels, steps, MOST);
    for (int64_t i = 0; i < b; ++i)
      if (inside) {
        if (channels[i] == channel)
          spontaneous++;
        else
          other_without++;
      }
  }
  printf("channel %d, 20 stims of %g: %lld spikes on it in the 20 ms after "
         "them, %lld without the stims\n",
         channel, (double)amount, (long long)evoked, (long long)spontaneous);
  printf("the other channels in the same windows: %lld, %lld without\n",
         (long long)other_with, (long long)other_without);
  printf("stim response: %s\n", evoked > 2 * spontaneous + 10 ? "yes" : "NO");
  dish_destroy(with);
  dish_destroy(without);
  return 0;
}
