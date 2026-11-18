// Does paired stimulation change how one channel answers another? Two
// dishes of one seed, through the library's C interface (bridge.c):
//
//   probe   STDP off in both: a stim on A alone, and 300 ms later on C
//           alone (a channel never paired), each repeated; the spikes on
//           B in the 5 to 30 ms after each counted;
//   train   A, then B 10 ms later, every 200 ms, for `minutes` of the
//           dish's time: in the first dish with STDP on, in the second off;
//   probe   again, as before.
//
// The change of B's answer to A (after less before) in the dish that
// learned, against the one that did not, is the effect; the same for C
// says whether it is A's pairing or the dish as a whole. With each
// probe's count, the mean, its standard error, and Cohen's d of the
// change (the difference of means over the pooled deviation).
//
// Unless they are given, B and C are chosen before any training, on a
// dish of the seed: B the channel that answers A most 5 to 30 ms after a
// stim (a stim's own channel and those its axons reach answer within the
// first few ms: no synapse in between, nothing to learn), C the channel
// other than A whose stims B answers most. A is 27.
//
//   cc -O2 plasticity.c -L. -ldish -Wl,-rpath,. -lm -o plasticity
//   ./plasticity [minutes [seed [A [B C]]]]
#include <math.h>
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
  float depress, tau_recover, hear_reach, tau_syn;
  float home_rate, tau_home, home_gain;
  float axon, fast, conduct;
  int32_t modulated;
  float tau_elig, tau_reward, reward_gain;
  int32_t reward_channel, punish_channel;
  int32_t hard;
} dish_params;
typedef struct dish dish;

void dish_defaults(dish_params *p);
dish *dish_create(const dish_params *p);
void dish_destroy(dish *d);
void dish_stimulate(dish *d, int32_t channel, float amount);
int64_t dish_step(dish *d, int32_t n);
int64_t dish_spikes(dish *d, int32_t *channel, int32_t *step, int64_t max);
void dish_set_stdp(dish *d, int32_t on);
uint64_t dish_checksum(dish *d);

enum { MOST = 1 << 16, PROBES = 100 };
static int32_t channels[MOST], steps[MOST];
static int A = 27, B = -1, C = -1;
static const float AMOUNT = 1.0f;

// Steps n, counting the spikes on channel `on` from step `from` to below
// `to` (counted from now).
static int64_t run(dish *d, int n, int on, int from, int to) {
  int64_t counted = 0;
  for (int t = 0; t < n; ++t) {
    dish_step(d, 1);
    int64_t got = dish_spikes(d, channels, steps, MOST);
    if (t >= from && t < to)
      for (int64_t i = 0; i < got; ++i)
        counted += channels[i] == on;
  }
  return counted;
}

typedef struct { double mean, se, sd; } stats;
static stats of(const int64_t *x, int n) {
  double m = 0, v = 0;
  for (int i = 0; i < n; ++i) m += x[i];
  m /= n;
  for (int i = 0; i < n; ++i) v += (x[i] - m) * (x[i] - m);
  v /= n - 1;
  return (stats){m, sqrt(v / n), sqrt(v)};
}

// B's answers to A and to C, PROBES each, with STDP off.
static void probe(dish *d, int64_t *to_a, int64_t *to_c) {
  dish_set_stdp(d, 0);
  for (int k = 0; k < PROBES; ++k) {
    dish_stimulate(d, A, AMOUNT);
    to_a[k] = run(d, 300, B, 5, 30);
    dish_stimulate(d, C, AMOUNT);
    to_c[k] = run(d, 300, B, 5, 30);
  }
}

static void train(dish *d, int stdp, int pairs) {
  dish_set_stdp(d, stdp);
  for (int k = 0; k < pairs; ++k) {
    dish_stimulate(d, A, AMOUNT);
    run(d, 10, -1, 0, 0);
    dish_stimulate(d, B, AMOUNT);
    run(d, 190, -1, 0, 0);
  }
}

// The spikes of each channel 5 to 30 ms after a stim on `from`, over
// `probes` stims 300 ms apart, into `answers`.
static void answers_to(dish *d, int from, int probes, int64_t *answers) {
  for (int c = 0; c < 64; ++c) answers[c] = 0;
  for (int k = 0; k < probes; ++k) {
    dish_stimulate(d, from, AMOUNT);
    for (int t = 0; t < 300; ++t) {
      dish_step(d, 1);
      int64_t got = dish_spikes(d, channels, steps, MOST);
      if (t >= 5 && t < 30)
        for (int64_t i = 0; i < got; ++i) answers[channels[i]] += 1;
    }
  }
}

static void choose(int64_t seed) {
  dish_params p;
  dish_defaults(&p);
  p.seed = seed;
  p.stdp = 0;
  dish *d = dish_create(&p);
  run(d, 1000, -1, 0, 0);
  int64_t answers[64], best = -1;
  answers_to(d, A, PROBES, answers);
  for (int c = 0; c < 64; ++c)
    if (c != A && answers[c] > best) best = answers[c], B = c;
  best = -1;
  for (int c = 0; c < 64; ++c) {
    if (c == A || c == B) continue;
    int64_t to[64];
    answers_to(d, c, 20, to);
    if (to[B] > best) best = to[B], C = c;
  }
  dish_destroy(d);
}

static double cohen(stats before, stats after) {
  double pooled = sqrt((before.sd * before.sd + after.sd * after.sd) / 2);
  return pooled > 0 ? (after.mean - before.mean) / pooled : 0;
}

int main(int argc, char **argv) {
  double minutes = argc > 1 ? atof(argv[1]) : 3;
  int64_t seed = argc > 2 ? atoll(argv[2]) : 1;
  if (argc > 3) A = atoi(argv[3]);
  if (argc > 5) {
    B = atoi(argv[4]);
    C = atoi(argv[5]);
  } else {
    choose(seed);
  }
  int pairs = (int)(minutes * 60 * 1000 / 200);
  printf("seed %lld: A %d, B %d, C %d; %d pairings (A, B 10 ms later) in %g "
         "minutes; %d probes of each before and after\n",
         (long long)seed, A, B, C, pairs, minutes, PROBES);
  static int64_t before_a[2][PROBES], before_c[2][PROBES],
      after_a[2][PROBES], after_c[2][PROBES];
  for (int learns = 1; learns >= 0; --learns) {
    dish_params p;
    dish_defaults(&p);
    p.seed = seed;
    dish *d = dish_create(&p);
    run(d, 1000, -1, 0, 0);  // a second to settle
    probe(d, before_a[learns], before_c[learns]);
    train(d, learns, pairs);
    probe(d, after_a[learns], after_c[learns]);
    stats ba = of(before_a[learns], PROBES), aa = of(after_a[learns], PROBES);
    stats bc = of(before_c[learns], PROBES), ac = of(after_c[learns], PROBES);
    printf("%s: B after A %.2f (se %.2f) -> %.2f (se %.2f), d %+.2f; "
           "B after C %.2f (se %.2f) -> %.2f (se %.2f), d %+.2f; checksum %016llx\n",
           learns ? "STDP on " : "STDP off", ba.mean, ba.se, aa.mean, aa.se,
           cohen(ba, aa), bc.mean, bc.se, ac.mean, ac.se, cohen(bc, ac),
           (unsigned long long)dish_checksum(d));
    dish_destroy(d);
  }
  // The effect: the change with STDP less the change without, per probe.
  for (int which = 0; which < 2; ++which) {
    int64_t *b1 = which ? before_c[1] : before_a[1], *a1 = which ? after_c[1] : after_a[1];
    int64_t *b0 = which ? before_c[0] : before_a[0], *a0 = which ? after_c[0] : after_a[0];
    int64_t learned[PROBES], control[PROBES];
    for (int k = 0; k < PROBES; ++k) {
      learned[k] = a1[k] - b1[k];
      control[k] = a0[k] - b0[k];
    }
    stats l = of(learned, PROBES), c = of(control, PROBES);
    double diff = l.mean - c.mean, se = sqrt(l.se * l.se + c.se * c.se);
    printf("B after %s: change with STDP %+.2f, without %+.2f: effect %+.2f "
           "spikes a probe (se %.2f, %.1f se)\n",
           which ? "C (unpaired)" : "A (paired)", l.mean, c.mean, diff, se,
           se > 0 ? diff / se : 0);
  }
  return 0;
}
