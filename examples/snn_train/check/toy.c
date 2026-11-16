// The toy task of examples/snn_train/toy.ent trained in plain C: the same
// network, the same numbers at random (wyhash), the same e-prop, every
// sum in the order ent makes it. It prints what toy.ent prints, so the
// two outputs are compared byte for byte (test/Integration/snn_train.test):
// every loss, every count and the checksum of the weights, which holds
// the bits of every weight.
//
//   cc -O2 -ffp-contract=off toy.c -lm -o toy_c && ./toy_c
//
// The order: a neuron's synapses come in by source, the hidden neurons
// (ent spawns them first, their archetype comes before the inputs') in
// the order they were made, then the inputs; of one source, in the order
// they were connected. exp and log are worked out in double and rounded
// to float, as devices/math.ent does (it is within an ulp in double, so
// the float is the same but for a value right at a rounding boundary).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// devices/wyhash.ent
static uint64_t wymix(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)a * b;
  return (uint64_t)(r >> 64) ^ (uint64_t)r;
}
static uint64_t wyhash64(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)(a ^ 0x2d358dccaa6c78a5ull) * (b ^ 0x8bb84b93962eacc9ull);
  uint64_t hi = (uint64_t)(r >> 64), lo = (uint64_t)r;
  return wymix(lo ^ 0x2d358dccaa6c78a5ull, hi ^ 0x8bb84b93962eacc9ull);
}
static uint64_t wyrand(uint64_t key, int64_t n) {
  uint64_t s = key + ((uint64_t)n + 1) * 0x2d358dccaa6c78a5ull;
  return wymix(s, s ^ 0x8bb84b93962eacc9ull);
}
static double wyrand_unit(uint64_t key, int64_t n) {
  return (double)(wyrand(key, n) >> 11) / 9007199254740992.0;
}
static uint64_t wyrand_below(uint64_t key, int64_t n, uint64_t count) {
  return (uint64_t)(((__uint128_t)wyrand(key, n) * count) >> 64);
}
static float normal(uint64_t key, int64_t n) {
  double a = wyrand_unit(key, n) + 1.0 / 9007199254740992.0;
  double b = wyrand_unit(key, n + 1);
  return (float)(sqrt(-2.0 * log(a)) * cos(6.283185307179586 * b));
}
// A float as ent puts it into a text: six decimals, rounded, without the
// zeros at the end but one.
static const char *decimals(float x) {
  static char out[64];
  snprintf(out, sizeof out, "%.6f", (double)x);
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == '0' && out[n - 2] != '.') out[--n] = 0;
  return out;
}
static float expf_(float x) { return (float)exp((double)x); }
static float logf_(float x) { return (float)log((double)x); }

enum { GROUP = 10, PHASE = 10, QUIET = 10, INPUTS = 2 * GROUP, HIDDEN = 32,
       CLASSES = 2, STEPS = 2 * PHASE + QUIET, WINDOW = QUIET, TRAIN = 200,
       TEST = 200, EPOCHS = 30, BATCH = 10, UNITS = HIDDEN + INPUTS };
static const double HIGH = 0.4, LOW = 0.02;
static const float ALPHA = 0.95f, KAPPA = 0.9f, THR = 1.0f, GAMMA = 0.3f,
                   RATE = 0.01f, W_IN = 1.0f, W_REC = 0.5f, DENSITY = 0.5f;
static const int64_t SEED = 1;
static const float SCALE = 1.0f / (float)WINDOW;

// Units 0..HIDDEN-1 are the hidden neurons, HIDDEN.. the inputs.
static float fired[UNITS], trace_[UNITS];
static float v[HIDDEN], input[HIDDEN], psi[HIDDEN], learn_[HIDDEN], out_trace[HIDDEN];
static float y[CLASSES], bias[CLASSES], btrace[CLASSES], rgrad[CLASSES], err[CLASSES], vote[CLASSES];

// Synapses into each neuron, in ent's order.
typedef struct { int pre; float weight, elig, grad, m, v; } Syn;
static Syn syn[HIDDEN][UNITS];
static int nsyn[HIDDEN];
static float wout[HIDDEN][CLASSES], gout[HIDDEN][CLASSES], mout[HIDDEN][CLASSES],
    vout[HIDDEN][CLASSES], bm[CLASSES], bv[CLASSES];

// Adam (learn.ent's apply): its decays and eps, b1^t, b2^t.
static const float B1 = 0.9f, B2 = 0.999f, EPS = 1.0e-8f;
static float p1 = 1.0f, p2 = 1.0f;
static float c1, c2, a1, a2;
static float adam(float w, float *m, float *v, float g) {
  float mm = B1 * *m + a1 * g;
  float vv = B2 * *v + a2 * (g * g);
  *m = mm;
  *v = vv;
  return w - RATE * (mm / c1) / (sqrtf(vv / c2) + EPS);
}

static void setup(void) {
  uint64_t key = wyhash64((uint64_t)SEED, 0x5eed);
  int64_t n = 0;
  // Connected in toy.ent's order; kept per target, then each target's
  // put in order by source (a stable sort: hidden first, then inputs).
  static Syn made[HIDDEN][UNITS];
  static int nmade[HIDDEN];
  float w_in = W_IN / sqrtf((float)INPUTS);
  for (int c = 0; c < INPUTS; c++)
    for (int j = 0; j < HIDDEN; j++) {
      made[j][nmade[j]++] = (Syn){HIDDEN + c, w_in * normal(key, n), 0, 0, 0, 0};
      n += 2;
    }
  float w_rec = W_REC / sqrtf(DENSITY * (float)HIDDEN);
  for (int i = 0; i < HIDDEN; i++)
    for (int j = 0; j < HIDDEN; j++) {
      if (i != j && (float)wyrand_unit(key, n) < DENSITY)
        made[j][nmade[j]++] = (Syn){i, w_rec * normal(key, n + 1), 0, 0, 0, 0};
      n += 3;
    }
  float w_out = 1.0f / sqrtf((float)HIDDEN);
  for (int j = 0; j < HIDDEN; j++)
    for (int k = 0; k < CLASSES; k++) {
      wout[j][k] = w_out * normal(key, n);
      n += 2;
    }
  for (int j = 0; j < HIDDEN; j++) {
    nsyn[j] = 0;
    for (int u = 0; u < UNITS; u++)
      for (int s = 0; s < nmade[j]; s++)
        if (made[j][s].pre == u) syn[j][nsyn[j]++] = made[j][s];
  }
}

static void rest(void) {
  memset(fired, 0, sizeof fired);
  memset(trace_, 0, sizeof trace_);
  memset(v, 0, sizeof v);
  memset(input, 0, sizeof input);
  memset(psi, 0, sizeof psi);
  memset(learn_, 0, sizeof learn_);
  memset(out_trace, 0, sizeof out_trace);
  memset(y, 0, sizeof y);
  memset(btrace, 0, sizeof btrace);
  memset(err, 0, sizeof err);
  memset(vote, 0, sizeof vote);
}

int main(void) {
  setup();
  float loss = 0, trained_loss = 0;
  int correct = 0, seen = 0, trained_correct = 0, trained_seen = 0;
  for (int epoch = 0; epoch < EPOCHS; epoch++) {
    for (int phase_of = 0; phase_of < 2; phase_of++) {
      int training = phase_of == 0;
      int count = training ? TRAIN : TEST;
      for (int sample = 0; sample < count; sample++) {
        int64_t id = training ? (int64_t)epoch * TRAIN + sample : 1000000000 + sample;
        int label = (int)wyrand_below(wyhash64((uint64_t)SEED, (uint64_t)id), 0, 2);
        rest();
        for (int step = 0; step < STEPS; step++) {
          int on = step >= STEPS - WINDOW;
          // encode
          int phase = step / PHASE;
          uint64_t at = wyhash64(wyhash64((uint64_t)SEED + 1, (uint64_t)id), (uint64_t)step);
          for (int c = 0; c < INPUTS; c++) {
            int group = c / GROUP;
            double chance = LOW;
            if (phase == 0 && group == label) chance = HIGH;
            if (phase == 1 && group != label) chance = HIGH;
            fired[HIDDEN + c] = wyrand_unit(at, c) < chance ? 1.0f : 0.0f;
          }
          // propagate
          for (int j = 0; j < HIDDEN; j++) {
            float sum = 0.0f;
            for (int s = 0; s < nsyn[j]; s++) sum += syn[j][s].weight * fired[syn[j][s].pre];
            input[j] = sum;
          }
          // traces
          for (int u = 0; u < UNITS; u++) trace_[u] = ALPHA * trace_[u] + fired[u];
          // integrate
          for (int j = 0; j < HIDDEN; j++) {
            float vv = ALPHA * v[j] + input[j];
            float near = 1.0f - fabsf(vv - THR) / THR;
            psi[j] = GAMMA * fmaxf(near, 0.0f) / THR;
            if (vv >= THR) {
              fired[j] = 1.0f;
              vv -= THR;
            } else {
              fired[j] = 0.0f;
            }
            v[j] = vv;
            out_trace[j] = KAPPA * out_trace[j] + fired[j];
          }
          // read_out
          for (int k = 0; k < CLASSES; k++) {
            float sum = 0.0f;
            for (int j = 0; j < HIDDEN; j++) sum += wout[j][k] * fired[j];
            y[k] = KAPPA * y[k] + sum + bias[k];
          }
          // soft_max
          float top = -1.0e30f;
          for (int k = 0; k < CLASSES; k++) top = fmaxf(top, y[k]);
          float total = 0.0f;
          for (int k = 0; k < CLASSES; k++) total += expf_(y[k] - top);
          // error
          for (int k = 0; k < CLASSES; k++) {
            if (on) {
              float p = expf_(y[k] - top) / total;
              if (k == label) {
                err[k] = (p - 1.0f) * SCALE;
                loss -= logf_(fmaxf(p, 1.0e-30f));
              } else {
                err[k] = p * SCALE;
              }
              vote[k] += y[k];
            } else {
              err[k] = 0.0f;
            }
          }
          if (training) {
            // broadcast
            for (int j = 0; j < HIDDEN; j++) {
              float sum = 0.0f;
              for (int k = 0; k < CLASSES; k++) sum += wout[j][k] * err[k];
              learn_[j] = sum;
            }
            // learn
            for (int j = 0; j < HIDDEN; j++)
              for (int s = 0; s < nsyn[j]; s++) {
                Syn *e = &syn[j][s];
                float elig = KAPPA * e->elig + psi[j] * trace_[e->pre];
                e->elig = elig;
                e->grad += learn_[j] * elig;
              }
            // learn_out
            for (int k = 0; k < CLASSES; k++) {
              for (int j = 0; j < HIDDEN; j++) gout[j][k] += err[k] * out_trace[j];
              btrace[k] = KAPPA * btrace[k] + 1.0f;
              rgrad[k] += err[k] * btrace[k];
            }
          }
        }
        // score
        float best = -1.0e30f;
        for (int k = 0; k < CLASSES; k++) best = fmaxf(best, vote[k]);
        int pick = -1;
        for (int k = 0; k < CLASSES; k++)
          if (vote[k] == best) pick = k;
        if (pick == label) correct++;
        seen++;
        // forget, then apply after a batch
        if (training)
          for (int j = 0; j < HIDDEN; j++)
            for (int s = 0; s < nsyn[j]; s++) syn[j][s].elig = 0.0f;
        if (training && ((sample + 1) % BATCH == 0 || sample + 1 == TRAIN)) {
          p1 *= B1;
          p2 *= B2;
          c1 = 1.0f - p1;
          c2 = 1.0f - p2;
          a1 = 1.0f - B1;
          a2 = 1.0f - B2;
          for (int j = 0; j < HIDDEN; j++)
            for (int s = 0; s < nsyn[j]; s++) {
              Syn *e = &syn[j][s];
              e->weight = adam(e->weight, &e->m, &e->v, e->grad);
              e->grad = 0.0f;
            }
          for (int k = 0; k < CLASSES; k++) {
            for (int j = 0; j < HIDDEN; j++) {
              wout[j][k] = adam(wout[j][k], &mout[j][k], &vout[j][k], gout[j][k]);
              gout[j][k] = 0.0f;
            }
            bias[k] = adam(bias[k], &bm[k], &bv[k], rgrad[k]);
            rgrad[k] = 0.0f;
          }
        }
      }
      if (training) {
        trained_loss = loss;
        trained_correct = correct;
        trained_seen = seen;
      } else {
        float mean = trained_loss / (float)(trained_seen * WINDOW);
        printf("epoch %d loss %s train %d/%d test %d/%d\n", epoch + 1, decimals(mean),
               trained_correct, trained_seen, correct, seen);
      }
      loss = 0.0f;
      correct = 0;
      seen = 0;
    }
  }
  int64_t bits = 0;
  for (int j = 0; j < HIDDEN; j++) {
    float sum = 0.0f;
    for (int s = 0; s < nsyn[j]; s++) sum += syn[j][s].weight;
    uint32_t b;
    memcpy(&b, &sum, 4);
    bits += b;
  }
  for (int k = 0; k < CLASSES; k++) {
    float sum = bias[k];
    for (int j = 0; j < HIDDEN; j++) sum += wout[j][k];
    uint32_t b;
    memcpy(&b, &sum, 4);
    bits += b;
  }
  printf("weights %lld\n", (long long)bits);
  return 0;
}
