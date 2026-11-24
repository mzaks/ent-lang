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
// 12 tiles as food, ahead of the pellets. DISH_FLEE=1 tells the way out
// when a ghost is near (plan_escape; 2 only when the food's way is not
// safe). DISH_ORACLE=1, a test, takes the way told at once (no dish
// in the loop). DISH_PICK=0 keeps the sensory
// electrodes below; 1 (the default) picks them by how the dish answers
// (pick_electrodes).
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
// ?prey=, ?pick=, ?path=, ?flee= stand for DISH_SEED, DISH_STIM, DISH_GHOSTS,
// DISH_MOTOR, DISH_DECODER, DISH_PREY, DISH_PICK, DISH_PATH and DISH_FLEE.
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

static int FOOD[4] = {37, 44, 26, 19};    // right, down, left, up
// A ghost's way is stimulated beside the motor region of the opposite way
// (a ghost on the right, the electrode next to the left region), so the
// layout itself turns him away: the dish is not taught what a ghost is.
static int DANGER[4] = {34, 20, 29, 43};
// DISH_GHOSTS=-1: beside the same way's region instead (towards him),
// 0: no ghost stims.
static const int TOWARDS[4] = {29, 43, 34, 20};
static int ghosts = 1, inner = 1, decoder = 1, prey = 1, pick = 1;
#define PREY_RANGE 12
static int role[64];
static float motor[4], base[4], activity[64];
static int food_dc, food_dr, food_d, ghost_dc, ghost_dr, ghost_d, prey_dc, prey_dr, prey_d;
static double food_due, danger_due, now_ms;
static int quiet;  // DISH_STIM=0: no stims, the dish's own activity alone

// The motor spikes of each region in the `ms` milliseconds to come.
static void count_motor(int ms, float out[4]) {
  int32_t ch[4096], st[4096];
  for (int k = 0; k < 4; ++k) out[k] = 0;
  for (int i = 0; i < ms; ++i) {
    step(dish, 1);
    int64_t got;
    while ((got = spikes(dish, ch, st, 4096)) > 0)
      for (int64_t j = 0; j < got; ++j)
        if (role[ch[j]] >= 0 && role[ch[j]] < 4) out[role[ch[j]]] += 1.0f;
  }
}

// DISH_PICK=1 (the default): which sensory electrode says what, from how
// this dish answers. Each of the middle 16 is pulsed alone PICK_PULSES
// times, PICK_GAP ms apart, and the motor spikes of each region in the
// 20 ms after are counted, less those of the 20 ms before; an electrode's
// lean to a region is its answer there less its mean answer in the
// others. Then, strongest lean first, each of the 8 roles takes an
// electrode on its side of the middle: the food of a way one on that
// way's side leaning to that way's region, a ghost's one on the opposite
// side leaning away from him. (Giving the weakest role the best it can
// get instead scored no better: 1871 against 1955 a game, 8 seeds.) The dish is not taught; the host chooses where
// to say things, as it chose the electrodes in Pong (zones.py layouts).
#define PICK_PULSES 4
#define PICK_GAP 100
static void pick_electrodes(void) {
  float lean[64][4] = {{0}};
  for (int p = 0; p < PICK_PULSES; ++p)
    for (int r = 2; r <= 5; ++r)
      for (int c = 2; c <= 5; ++c) {
        int e = r * 8 + c;
        float before[4], after[4];
        count_motor(PICK_GAP - 20, before);
        count_motor(20, before);
        stimulate(dish, e, 1.0f);
        count_motor(20, after);
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += after[k] - before[k];
        for (int k = 0; k < 4; ++k) {
          float a = after[k] - before[k];
          lean[e][k] += a - (sum - a) / 3;
        }
      }
  int used[64] = {0}, done[8] = {0};
  for (int round = 0; round < 8; ++round) {
    int best_slot = -1, best_e = -1;
    float best = -1e30f;
    for (int slot = 0; slot < 8; ++slot) {
      if (done[slot]) continue;
      int w = slot % 4, toward = slot < 4 ? w : (w + 2) % 4;   // food: its way; danger: away
      for (int r = 2; r <= 5; ++r)
        for (int c = 2; c <= 5; ++c) {
          int e = r * 8 + c;
          int side = toward == 0 ? c == 5 : toward == 1 ? r == 5 : toward == 2 ? c == 2 : r == 2;
          if (!side || used[e]) continue;
          if (lean[e][toward] > best) { best = lean[e][toward]; best_slot = slot; best_e = e; }
        }
    }
    if (best_slot < 0) break;
    done[best_slot] = used[best_e] = 1;
    if (best_slot < 4) FOOD[best_slot] = best_e;
    else DANGER[best_slot - 4] = best_e;
  }
  if (getenv("DISH_DEBUG")) {
    fprintf(stderr, "picked food %d %d %d %d danger %d %d %d %d\n", FOOD[0], FOOD[1], FOOD[2], FOOD[3],
            DANGER[0], DANGER[1], DANGER[2], DANGER[3]);
    for (int e = 0; e < 64; ++e)
      if (lean[e][0] || lean[e][1] || lean[e][2] || lean[e][3])
        fprintf(stderr, "  e%d lean R %.1f D %.1f L %.1f U %.1f\n", e, lean[e][0] / PICK_PULSES,
                lean[e][1] / PICK_PULSES, lean[e][2] / PICK_PULSES, lean[e][3] / PICK_PULSES);
  }
}

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
  const char *pk = getenv("DISH_PICK");
  pick = pk ? atoi(pk) : 1;
  int lo = inner ? 1 : 0, hi = inner ? 6 : 7;   // the rows and columns the regions take
  for (int c = 0; c < 64; ++c) {
    int r = c / 8, k = c % 8;
    role[c] = -1;
    if (r >= lo && r <= 1 && k >= 2 && k <= 5) role[c] = 3;
    if (r >= 6 && r <= hi && k >= 2 && k <= 5) role[c] = 1;
    if (k >= lo && k <= 1 && r >= 2 && r <= 5) role[c] = 2;
    if (k >= 6 && k <= hi && r >= 2 && r <= 5) role[c] = 0;
  }
  step(dish, 2000);  // two seconds to settle
  int32_t ch[4096], st[4096];
  while (spikes(dish, ch, st, 4096) == 4096) {}
  if (pick)
    pick_electrodes();
  for (int i = 0; i < 4; ++i) {
    role[FOOD[i]] = 4;
    role[DANGER[i]] = 5;
  }
}

// The maze, when the game gives it (pilot_open): then where something is
// is told as the first step of the shortest way to it through the maze
// and how many steps that is, not as the crow flies, which sent him to
// and fro past the turn up to a pellet behind a wall. DISH_PATH: 1 (the
// default) the pellets and frightened ghosts so, 3 the other ghosts too
// (they then warn him later: worse), 0 all as the crow flies.
#define COLS 28
#define ROWS 31
static unsigned char open_tile[ROWS][COLS];
static int have_maze, by_path = -1, pac_col, pac_row, pac_dir = 4;
static int dist[ROWS][COLS], came[ROWS][COLS], first[ROWS][COLS];
static const int DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};

void ent_dishpilot_pilot_open(int32_t col, int32_t row, bool open) {
  if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
    open_tile[row][col] = open;
    have_maze = 1;
  }
}

// Each frame, where he is: the steps from there to every tile, and the
// first of them, breadth first (the tunnel wraps).
void ent_dishpilot_pilot_at(int32_t col, int32_t row, int32_t dir) {
  pac_dir = dir;
  if (by_path < 0) {
    const char *p = getenv("DISH_PATH");
    by_path = p ? atoi(p) : 1;
  }
  pac_col = col;
  pac_row = row;
  if (!have_maze || !by_path || col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
  static int queue[ROWS * COLS];
  for (int r = 0; r < ROWS; ++r)
    for (int c = 0; c < COLS; ++c) dist[r][c] = -1;
  int head = 0, tail = 0;
  dist[row][col] = 0;
  came[row][col] = -1;
  queue[tail++] = row * COLS + col;
  while (head < tail) {
    int at = queue[head++], r = at / COLS, c = at % COLS;
    for (int w = 0; w < 4; ++w) {
      int nr = r + DY[w], nc = (c + DX[w] + COLS) % COLS;
      if (nr < 0 || nr >= ROWS || !open_tile[nr][nc] || dist[nr][nc] >= 0) continue;
      dist[nr][nc] = dist[r][c] + 1;
      came[nr][nc] = w;   // the last step to it
      first[nr][nc] = at == row * COLS + col ? w : first[r][c];   // and the first
      queue[tail++] = nr * COLS + nc;
    }
  }
}

// Where something is from him, as (dc, dr) whose larger part is the way
// to go and whose size is the steps: by the maze when it is known.
static int path_to(int r, int c, int32_t *dc, int32_t *dr);
static int locate(int32_t *dc, int32_t *dr, int what) {
  if (!have_maze || !(by_path & what)) return abs(*dc) + abs(*dr);
  int c = ((pac_col + *dc) % COLS + COLS) % COLS, r = pac_row + *dr;
  return path_to(r, c, dc, dr);
}

static int path_to(int r, int c, int32_t *dc, int32_t *dr) {
  if (r < 0 || r >= ROWS || dist[r][c] < 0) return -1;   // not reachable
  int d = dist[r][c];
  if (d == 0) return 0;
  // The steps there, walked back from it.
  int steps[ROWS * COLS], n = 0;
  while (came[r][c] >= 0) {
    int w = came[r][c];
    steps[n++] = w;
    r -= DY[w];
    c = (c - DX[w] + COLS) % COLS;
  }
  // Going that way already: the next turn on it, told early, for the game
  // keeps a way asked for until it opens (the dish answers in about the
  // time he takes a tile); else the first step.
  int w = steps[n - 1];
  if (w == pac_dir)
    for (int k = n - 2; k >= 0; --k)
      if (steps[k] != w) { w = steps[k]; break; }
  *dc = DX[w] * d;
  *dr = DY[w] * d;
  return d;
}

// Self-preservation (DISH_FLEE=1; 0, the default, leaves it out): with a ghost within
// FLEE_RANGE steps, the food told is not the nearest pellet but the way
// out. The tiles he reaches before any ghost can (by the maze, ghosts
// free to turn) are safe; of his first steps the one that opens onto the
// most of them is the escape, and he is told the way to the nearest
// pellet on it, or, with none there, to its tile the ghosts reach last
// after him. Greedy alone, he ran from a ghost only where a pellet lay.
// Measured (24 Nov, 20 min, 8 seeds, paired): played by the host
// (DISH_ORACLE=1) fleeing lifts the score 2520 -> 3081, but with the dish
// it is a wash (3266 -> 3261, +31 +- 214; DISH_FLEE=2 +38 +- 167): the
// ghost stims' turn-away already gives most of it (the dish beats the
// host's greedy play, 3266 vs 2520), and a way out told three tiles from
// a ghost comes about a tile too late from the dish.
// DISH_MARGIN: the steps he must be ahead by (1; 2 and 3 were worse).
#define FLEE_RANGE 8
#define ROOM_MIN 12
static int flee = -1, n_hunters, n_pellets;
static int hunter[4 * 4], pellet_at[ROWS * COLS];
static int keep_r = -1, keep_c;
static int flee_d, flee_dc, flee_dr, fleeing, prey_r, prey_c, food_r = -1, food_c;

void ent_dishpilot_pilot_begin(void) {
  open_dish();
  food_d = ghost_d = prey_d = 1 << 30;
  n_hunters = n_pellets = 0;
  food_r = -1;
}

static void plan_escape(void) {
  int was = fleeing;
  fleeing = 0;
  if (flee < 0) {
    const char *f = getenv("DISH_FLEE");
    flee = f ? atoi(f) : 0;
  }
  static int margin = -1;
  if (margin < 0) { const char *m = getenv("DISH_MARGIN"); margin = m ? atoi(m) : 1; }
  if (!flee || !have_maze || !by_path || ghost_d > FLEE_RANGE || !n_hunters) return;
  static int gdist[ROWS][COLS], queue[ROWS * COLS];
  int head = 0, tail = 0;
  for (int r = 0; r < ROWS; ++r)
    for (int c = 0; c < COLS; ++c) gdist[r][c] = -1;
  for (int i = 0; i < n_hunters; ++i) {
    int at = hunter[i], r = at / COLS, c = at % COLS;
    if (r < 0 || r >= ROWS || gdist[r][c] >= 0) continue;
    gdist[r][c] = 0;
    queue[tail++] = at;
  }
  while (head < tail) {
    int at = queue[head++], r = at / COLS, c = at % COLS;
    for (int w = 0; w < 4; ++w) {
      int nr = r + DY[w], nc = (c + DX[w] + COLS) % COLS;
      if (nr < 0 || nr >= ROWS || !open_tile[nr][nc] || gdist[nr][nc] >= 0) continue;
      gdist[nr][nc] = gdist[r][c] + 1;
      queue[tail++] = nr * COLS + nc;
    }
  }
#define SAFE(r, c) (dist[r][c] > 0 && (gdist[r][c] < 0 || dist[r][c] + margin < gdist[r][c]))
  int room[4] = {0, 0, 0, 0};
  for (int r = 0; r < ROWS; ++r)
    for (int c = 0; c < COLS; ++c)
      if (SAFE(r, c)) room[first[r][c]]++;
  int b = 0;
  for (int w = 1; w < 4; ++w) if (room[w] > room[b]) b = w;
  if (!room[b]) return;   // nowhere safe: as before
  // DISH_FLEE=2: the nearest pellet still, while he gets there first and
  // its way opens onto room enough (ROOM_MIN safe tiles); once fleeing,
  // he flees on until his way out is no longer safe.
  if (flee == 2 && !was && food_r >= 0 && SAFE(food_r, food_c) && room[first[food_r][food_c]] >= ROOM_MIN) return;
  // A frightened ghost he reaches first, on the way out: still the food.
  if (prey && prey_d <= PREY_RANGE && SAFE(prey_r, prey_c) && first[prey_r][prey_c] == b) return;
  int tr = -1, tc = -1, best = -1;
  // The way out once chosen is kept while it stays safe (chosen afresh
  // each frame, as the ghosts move, he turned to and fro between two).
  if (was && keep_r >= 0 && SAFE(keep_r, keep_c) && 2 * room[first[keep_r][keep_c]] >= room[b]) { tr = keep_r; tc = keep_c; }
  else for (int i = 0; i < n_pellets; ++i) {
    int r = pellet_at[i] / COLS, c = pellet_at[i] % COLS;
    if (SAFE(r, c) && first[r][c] == b && (best < 0 || dist[r][c] < best)) { best = dist[r][c]; tr = r; tc = c; }
  }
  if (tr < 0)
    for (int r = 0; r < ROWS; ++r)
      for (int c = 0; c < COLS; ++c) {
        if (!SAFE(r, c) || first[r][c] != b) continue;
        int lead = gdist[r][c] < 0 ? 1000 : gdist[r][c] - dist[r][c];
        if (lead > best) { best = lead; tr = r; tc = c; }
      }
#undef SAFE
  int32_t dc, dr;
  int d = path_to(tr, tc, &dc, &dr);
  keep_r = -1;
  if (d <= 0) return;
  keep_r = tr; keep_c = tc;
  fleeing = 1;
  flee_d = d; flee_dc = dc; flee_dr = dr;
}

void ent_dishpilot_pilot_pellet(int32_t dc, int32_t dr) {
  int at = (pac_row + dr) * COLS + ((pac_col + dc) % COLS + COLS) % COLS;
  if (n_pellets < ROWS * COLS && pac_row + dr >= 0 && pac_row + dr < ROWS) pellet_at[n_pellets++] = at;
  int d = locate(&dc, &dr, 1);
  if (d > 0 && d < food_d) { food_d = d; food_dc = dc; food_dr = dr; food_r = at / COLS; food_c = at % COLS; }
}

void ent_dishpilot_pilot_ghost(int32_t dc, int32_t dr, bool scared) {
  int at = (pac_row + dr) * COLS + ((pac_col + dc) % COLS + COLS) % COLS;
  if (!scared && n_hunters < 16) hunter[n_hunters++] = at;
  int d = locate(&dc, &dr, scared ? 1 : 2);
  if (d < 0) return;
  if (!scared && d < ghost_d) { ghost_d = d; ghost_dc = dc; ghost_dr = dr; }
  if (scared && d < prey_d) { prey_d = d; prey_dc = dc; prey_dr = dr; prey_r = at / COLS; prey_c = at % COLS; }
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
  plan_escape();   // a frame's: sense told where all is
  if (fleeing) { fd = flee_d; fdc = flee_dc; fdr = flee_dr; }
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
  int out = (big <= 0.0f || pull[best] <= 0.0f || pull[best] - pull[second] < 0.1f * big) ? 4 : best;
  // DISH_ORACLE=1 (a test, not the dish): the way told, taken at once.
  static int oracle = -1;
  if (oracle < 0) { const char *o = getenv("DISH_ORACLE"); oracle = o && atoi(o); }
  if (oracle) out = fd < (1 << 30) ? way(fdc, fdr) : 4;
  static double next_log;
  if (getenv("DISH_DEBUG") && now_ms >= next_log) {
    next_log = now_ms + 250;
    fprintf(stderr, "t %.2f food %d,%d d%d way%d hunt%d flee%d ghost d%d | raw %.1f %.1f %.1f %.1f | pull %.1f %.1f %.1f %.1f -> %d\n",
            now_ms / 1000, fdc, fdr, fd, way(fdc, fdr), hunt, fleeing, ghost_d, motor[0], motor[1], motor[2], motor[3],
            pull[0], pull[1], pull[2], pull[3], out);
  }
  return out;
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
