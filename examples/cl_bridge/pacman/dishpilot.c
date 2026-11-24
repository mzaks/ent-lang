// The dish as Pac-Man's pilot: libdish.so (examples/cl_bridge) loaded with
// dlopen, its own symbols kept to itself (RTLD_LOCAL | RTLD_DEEPBIND: the
// game is an ent world too). ENT_DISH_LIBRARY names the library (default
// build/cl_bridge/libdish.so), DISH_SEED the culture (1), DISH_AXON the
// axons' length (0.11, so stims do not drive the motor regions directly),
// DISH_STIM=0 leaves the stims out, DISH_GHOSTS=0 the ghosts' (-1: their
// electrodes beside the same way's region, towards them); PACMAN_FAST=1 asks the game to run
// as fast as it can. DISH_MOTOR=0 the motor regions two electrodes deep
// (as first built), 1 (the default) only the ring that touches the
// sensory electrodes (Pong's lesson: a region beside the electrodes that
// drive it answers them; one far off answers its own noise). DISH_DECODER=0
// reads the regions as they fire, 1 (the default) each against its own
// background (its average over 2 s), as Pong's paddle does. DISH_PREY=0
// leaves frightened ghosts out; 1 (the default) tells the nearest within
// 12 tiles as food, ahead of the pellets.
//
// Layout on the 8 x 8 grid (channel = row * 8 + col):
//   motor up    rows 0-1, cols 2-5      motor down  rows 6-7, cols 2-5
//   motor left  cols 0-1, rows 2-5      motor right cols 6-7, rows 2-5
//   (DISH_MOTOR=1: only row 1, row 6, col 1 and col 6 of those)
//   sensory, the middle 4 x 4: one electrode for each way the nearest
//   pellet is (food), beside that way's motor region, and one for each
//   way the nearest ghost is (danger), beside the opposite way's,
//   its stims at 4 Hz far to 40 Hz near (DishBrain's rate code).
// The way is the motor region with the most spikes, each averaged over
// 100 ms, if it leads the next by 10%.
//
// On a web page (ent build --web) there is no dlopen: the dish is its own
// WebAssembly module (examples/cl_bridge/web/build.sh's dish.mjs and
// dish.wasm, next to the page), loaded by dishpilot.pre.js before the game
// starts, and called through the functions below, which copy the spikes
// from its memory into the game's. The URL's ?seed= (any text, hashed as
// ent_dish.seed_of does), ?stim=0, ?ghosts=, ?motor=, ?decoder= and
// ?prey= stand for DISH_SEED, DISH_STIM, DISH_GHOSTS, DISH_MOTOR,
// DISH_DECODER and DISH_PREY.
#define _GNU_SOURCE
#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// bridge.c's dish_params, as check/dish_check.c copies it: keep in step.
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

static void (*defaults)(dish_params *);
static void *(*create)(const dish_params *);
static void (*stimulate)(void *, int32_t, float);
static int64_t (*step)(void *, int32_t);
static int64_t (*spikes)(void *, int32_t *, int32_t *, int64_t);
static void *dish;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <string.h>
EM_JS(int, web_dish_create, (double seed, double axon), {
  const D = Module.dish;
  const p = D._malloc(D._dish_params_size());
  D._dish_defaults(p);
  const set = (name, value) => {
    const bytes = new TextEncoder().encode(name + "\0"), at = D._malloc(bytes.length);
    D.HEAPU8.set(bytes, at);
    D._dish_params_set(p, at, value);
    D._free(at);
  };
  set("seed", seed);
  set("axon", axon);
  Module.dishOne = D._dish_create(p);
  D._free(p);
  Module.dishSpikes = D._malloc(8 * 4096);
  return Module.dishOne ? 1 : 0;
});
EM_JS(void, web_dish_stimulate, (int32_t channel, float amount), {
  Module.dish._dish_stimulate(Module.dishOne, channel, amount);
});
EM_JS(double, web_dish_step, (int32_t n), {
  return Number(Module.dish._dish_step(Module.dishOne, n));
});
EM_JS(int32_t, web_dish_spikes, (int32_t *channel, int32_t max), {
  const D = Module.dish, at = Module.dishSpikes;
  const got = Number(D._dish_spikes(Module.dishOne, at, at + 4 * 4096, BigInt(Math.min(max, 4096))));
  HEAP32.set(D.HEAP32.subarray(at / 4, at / 4 + got), channel / 4);
  return got;
});
static void web_defaults(dish_params *p) { memset(p, 0, sizeof *p); }
static void *web_create(const dish_params *p) {
  return web_dish_create((double)p->seed, p->axon) ? (void *)1 : NULL;
}
static void web_stimulate(void *d, int32_t channel, float amount) {
  (void)d;
  web_dish_stimulate(channel, amount);
}
static int64_t web_step(void *d, int32_t n) {
  (void)d;
  return (int64_t)web_dish_step(n);
}
static int64_t web_spikes(void *d, int32_t *channel, int32_t *step, int64_t max) {
  (void)d;
  (void)step;
  return web_dish_spikes(channel, (int32_t)(max < 4096 ? max : 4096));
}
#endif

static const int FOOD[4] = {37, 44, 26, 19};    // right, down, left, up
// A ghost's way is stimulated beside the motor region of the opposite way
// (a ghost on the right, the electrode next to the left region), so the
// layout itself turns him away: the dish is not taught what a ghost is.
static const int DANGER[4] = {34, 20, 29, 43};
// DISH_GHOSTS=-1: beside the same way's region instead (towards him),
// 0: no ghost stims.
static const int TOWARDS[4] = {29, 43, 34, 20};
static int ghosts = 1, inner = 1, decoder = 1, prey = 1;
#define PREY_RANGE 12
static int role[64];
static float motor[4], base[4], activity[64];
static int food_dc, food_dr, food_d, ghost_dc, ghost_dr, ghost_d, prey_dc, prey_dr, prey_d;
static double food_due, danger_due, now_ms;
static int quiet;  // DISH_STIM=0: no stims, the dish's own activity alone

static void open_dish(void) {
  if (dish)
    return;
#ifdef __EMSCRIPTEN__
  defaults = web_defaults;
  create = web_create;
  stimulate = web_stimulate;
  step = web_step;
  spikes = web_spikes;
#else
  const char *path = getenv("ENT_DISH_LIBRARY");
  void *lib = dlopen(path ? path : "build/cl_bridge/libdish.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!lib) {
    fprintf(stderr, "dishpilot: %s\n", dlerror());
    exit(1);
  }
  defaults = dlsym(lib, "dish_defaults");
  create = dlsym(lib, "dish_create");
  stimulate = dlsym(lib, "dish_stimulate");
  step = dlsym(lib, "dish_step");
  spikes = dlsym(lib, "dish_spikes");
#endif
  dish_params p;
  defaults(&p);
  const char *seed = getenv("DISH_SEED"), *axon = getenv("DISH_AXON");
  p.seed = seed ? atoll(seed) : 1;
  p.axon = axon ? atof(axon) : 0.11f;
  dish = create(&p);
  const char *stim = getenv("DISH_STIM");
  quiet = stim && atoi(stim) == 0;
  const char *g = getenv("DISH_GHOSTS");
  ghosts = g ? atoi(g) : 1;
  const char *m = getenv("DISH_MOTOR"), *dec = getenv("DISH_DECODER"), *pr = getenv("DISH_PREY");
  inner = m ? atoi(m) : 1;
  decoder = dec ? atoi(dec) : 1;
  prey = pr ? atoi(pr) : 1;
  int lo = inner ? 1 : 0, hi = inner ? 6 : 7;   // the rows and columns the regions take
  for (int c = 0; c < 64; ++c) {
    int r = c / 8, k = c % 8;
    role[c] = -1;
    if (r >= lo && r <= 1 && k >= 2 && k <= 5) role[c] = 3;
    if (r >= 6 && r <= hi && k >= 2 && k <= 5) role[c] = 1;
    if (k >= lo && k <= 1 && r >= 2 && r <= 5) role[c] = 2;
    if (k >= 6 && k <= hi && r >= 2 && r <= 5) role[c] = 0;
  }
  for (int i = 0; i < 4; ++i) {
    role[FOOD[i]] = 4;
    role[DANGER[i]] = 5;
  }
  step(dish, 2000);  // two seconds to settle
  int32_t ch[4096], st[4096];
  while (spikes(dish, ch, st, 4096) == 4096) {}
}

void ent_dishpilot_pilot_begin(void) {
  open_dish();
  food_d = ghost_d = prey_d = 1 << 30;
}

void ent_dishpilot_pilot_pellet(int32_t dc, int32_t dr) {
  int d = abs(dc) + abs(dr);
  if (d < food_d) { food_d = d; food_dc = dc; food_dr = dr; }
}

void ent_dishpilot_pilot_ghost(int32_t dc, int32_t dr, bool scared) {
  int d = abs(dc) + abs(dr);
  if (!scared && d < ghost_d) { ghost_d = d; ghost_dc = dc; ghost_dr = dr; }
  if (scared && d < prey_d) { prey_d = d; prey_dc = dc; prey_dr = dr; }
}

static int way(int dc, int dr) {
  if (abs(dc) >= abs(dr)) return dc >= 0 ? 0 : 2;
  return dr >= 0 ? 1 : 3;
}

static double rate(int d, int far) {
  double near = 1.0 - fmin(d, far) / (double)far;
  return 4.0 + 36.0 * near;  // Hz
}

int32_t ent_dishpilot_pilot_step(float ms) {
  open_dish();
  int n = (int)lroundf(fminf(ms, 50.0f));
  float decay = expf(-1.0f / 100.0f), base_decay = expf(-1.0f / 2000.0f);
  int32_t ch[4096], st[4096];
  // A frightened ghost near enough is the food, ahead of the pellets.
  int hunt = prey && prey_d <= PREY_RANGE;
  int fd = hunt ? prey_d : food_d, fdc = hunt ? prey_dc : food_dc, fdr = hunt ? prey_dr : food_dr;
  for (int i = 0; i < n; ++i) {
    now_ms += 1.0;
    if (!quiet && fd < (1 << 30) && now_ms >= food_due) {
      stimulate(dish, FOOD[way(fdc, fdr)], 1.0f);
      food_due = now_ms + 1000.0 / rate(fd, 30);
    }
    if (!quiet && ghosts != 0 && ghost_d <= 10 && now_ms >= danger_due) {
      stimulate(dish, (ghosts < 0 ? TOWARDS : DANGER)[way(ghost_dc, ghost_dr)], 1.0f);
      danger_due = now_ms + 1000.0 / rate(ghost_d, 10);
    }
    step(dish, 1);
    for (int k = 0; k < 4; ++k) {
      motor[k] *= decay;
      base[k] = base_decay * base[k] + (1 - base_decay) * motor[k];
    }
    for (int c = 0; c < 64; ++c) activity[c] *= decay;
    int64_t got;
    while ((got = spikes(dish, ch, st, 4096)) > 0)
      for (int64_t j = 0; j < got; ++j) {
        activity[ch[j]] += 1.0f;
        if (role[ch[j]] >= 0 && role[ch[j]] < 4) motor[role[ch[j]]] += 1.0f;
      }
  }
  // Each region's pull: its spikes, or (decoder 1) what it fires above its
  // own background; the way is the strongest if it leads the next by 10%
  // of the larger region's.
  float pull[4], big = 0.0f;
  for (int k = 0; k < 4; ++k) {
    pull[k] = decoder ? motor[k] - base[k] : motor[k];
    if (motor[k] > big) big = motor[k];
  }
  int best = 0, second = -1;
  for (int k = 1; k < 4; ++k) if (pull[k] > pull[best]) best = k;
  for (int k = 0; k < 4; ++k) if (k != best && (second < 0 || pull[k] > pull[second])) second = k;
  if (big <= 0.0f || pull[best] <= 0.0f || pull[best] - pull[second] < 0.1f * big) return 4;
  return best;
}

float ent_dishpilot_pilot_activity(int32_t channel) {
  return channel >= 0 && channel < 64 ? activity[channel] : 0.0f;
}

bool ent_dishpilot_pilot_fast(void) {
  static int fast = -1;
  if (fast < 0) {
    const char *f = getenv("PACMAN_FAST");
    fast = f && atoi(f) != 0;
  }
  return fast;
}

int32_t ent_dishpilot_pilot_role(int32_t channel) {
  open_dish();
  return channel >= 0 && channel < 64 ? role[channel] : -1;
}
