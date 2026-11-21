// pong.py's game in C, next to the dish (bridge.c), so that a host drives
// both in one call a frame: Python through ctypes (dish_pong.py) and a web
// page through WebAssembly (web/), the same games from the same seed.
//
// The `columns` layout and two of pong.py's conditions: the reward for
// each move (`moves`, cond 0) and for hits and misses (`reward`, cond 1),
// both with no feedback stims and a new ball at once after a miss. Unlike
// pong.py it does not go through cl-sdk: each tick (5 ms) the dish runs 5
// steps, then the game reads their spikes and its stims go in before the
// next tick; and the ball's paths come from its own generator, so a seed
// plays other games here than in pong.py. Its maths is plain arithmetic
// (no libm), so the native library and WebAssembly give the same bits.
//
//   dish *pong_make_dish(seed, gain, cond)   the dish with the study's
//       parameters (learn_study.py ... columns [moves] tau_elig=200
//       axon=0.11 hard=1): reward_gain = gain
//   pong *pong_create(dish, seed, cond, ego)  ego: the ball's place
//       relative to the paddle (pong.py's BALL_EGO)
//   int32_t pong_run(p, ticks, spikes, stims)  runs `ticks` ticks; adds
//       each channel's spikes to spikes[64]; stims[64] the most of each
//       channel's stims (1 a sensory stim, 2 a reward, 3 a punish pulse)
//   void pong_fixed_serves(p, base)   from the next serve on, each rally's
//       paths from a generator of its own, base and the rally's number: the
//       k-th rally starts the same whatever the paddle did before
//       (frozen_eval.py's evaluation; 0: off)
//   void pong_state(p, double s[12])   t, x, y, paddle, rallies, hits,
//       the last rally's hits, hits of all rallies, moves judged, moves
//       towards the ball, rallies after 5 minutes, their hits
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dish dish;
typedef struct dish_params dish_params;
// (bridge.c's, by declaration: the struct's layout is dish_params in
// bridge.c, filled there by dish_defaults.)
void dish_defaults(dish_params *p);
dish *dish_create(const dish_params *p);
void dish_stimulate(dish *d, int32_t channel, float amount);
int64_t dish_step(dish *d, int32_t n);
int64_t dish_spikes(dish *d, int32_t *channel, int32_t *step, int64_t max);
// (Set by name in bridge.c, so the layout is known only there.)
int32_t dish_params_set(dish_params *p, const char *name, double value);
int32_t dish_params_size(void);

#define TICKS 200
#define DT (1.0 / TICKS)
#define STEPS 5                // dish steps (1 ms) a tick
#define BALL_SPEED 0.5
#define PADDLE 0.3
#define PADDLE_SPEED 1.0
#define MOTOR_TAU 0.1
#define DEAD_ZONE 0.1
#define RPE_RATE 0.05
#define MOVE_S 0.1
#define EARLY_S 300.0
#define REWARD 28
#define PUNISH 36
#define QUARTER_PI 0.78539816339744830962

static const int32_t SENSORY[8] = {3, 12, 20, 27, 35, 44, 52, 59};

typedef struct pong {
  dish *d;
  uint64_t rng, fixed;
  int32_t cond, ego;
  double t, x, y, vx, vy, paddle, up, down, decay;
  double next_sense, judged_at, judged_paddle, expect, expect_move, start;
  int32_t hits, rallies, last_hits, moves_judged, moves_towards;
  int64_t hits_all, late_rallies, late_hits;
  int32_t channel[4096], step[4096];
  // This tick's stims: channel, amount, kind.
  int32_t n_out, out_ch[16], out_kind[16];
  float out_amount[16];
} pong;

// (splitmix64: the same numbers everywhere.)
static double next_unit(pong *p) {
  uint64_t z = (p->rng += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double uniform(pong *p, double a, double b) { return a + (b - a) * next_unit(p); }

// tan on [-pi/4, pi/4] and exp near 0 by their series: no libm, whose
// last bits differ from one C library to another.
static double tan_quarter(double a) {
  double a2 = a * a, s = a, c = 1.0, term_s = a, term_c = 1.0;
  for (int k = 1; k <= 12; ++k) {
    term_s *= -a2 / ((2 * k) * (2 * k + 1));
    term_c *= -a2 / ((2 * k - 1) * (2 * k));
    s += term_s;
    c += term_c;
  }
  return s / c;
}

static double exp_small(double v) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k <= 20; ++k) {
    term *= v / k;
    sum += term;
  }
  return sum;
}

static void aim(pong *p) {
  double angle = uniform(p, -QUARTER_PI, QUARTER_PI);
  p->vy = (p->vx < 0 ? -p->vx : p->vx) * tan_quarter(angle);
}

static void serve(pong *p) {
  if (p->fixed)
    p->rng = p->fixed + (uint64_t)p->rallies * 0xD1B54A32D192ED03ull;
  p->x = 1.0;
  p->y = uniform(p, 0.1, 0.9);
  p->vx = -BALL_SPEED;
  aim(p);
  p->hits = 0;
  p->start = p->t;
}

static void emit(pong *p, int32_t channel, float amount, int32_t kind) {
  if (p->n_out < 16) {
    p->out_ch[p->n_out] = channel;
    p->out_amount[p->n_out] = amount;
    p->out_kind[p->n_out] = kind;
    p->n_out++;
  }
}

// The reward less what was expected, as a pulse on REWARD or PUNISH.
static void outcome(pong *p, double got, int move) {
  double *expect = move ? &p->expect_move : &p->expect;
  double err = got - *expect;
  *expect += RPE_RATE * err;
  if (err != 0.0)
    emit(p, err > 0 ? REWARD : PUNISH, (float)(err > 0 ? err : -err), err > 0 ? 2 : 3);
}

dish *pong_make_dish(int64_t seed, double gain, int32_t cond) {
  dish_params *params = malloc((size_t)dish_params_size());
  if (!params)
    return NULL;
  dish_defaults(params);
  dish_params_set(params, "seed", (double)seed);
  dish_params_set(params, "axon", 0.11);
  dish_params_set(params, "hard", 1);
  dish_params_set(params, "modulated", 1);
  dish_params_set(params, "reward_channel", REWARD);
  dish_params_set(params, "punish_channel", PUNISH);
  dish_params_set(params, "reward_gain", gain);
  if (cond == 0)
    dish_params_set(params, "tau_elig", 200);
  dish *d = dish_create(params);
  free(params);
  return d;
}

pong *pong_create(dish *d, int64_t seed, int32_t cond, int32_t ego) {
  pong *p = calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->d = d;
  p->rng = (uint64_t)seed * 7919u + 1u;
  p->cond = cond;
  p->ego = ego;
  p->paddle = 0.5;
  p->expect = p->expect_move = 0.5;
  p->decay = exp_small(-DT / MOTOR_TAU);
  serve(p);
  return p;
}

void pong_destroy(pong *p) { free(p); }

void pong_fixed_serves(pong *p, int64_t base) { p->fixed = (uint64_t)base; }

// One tick of the game, with the spikes of the motor regions this tick.
static void game_tick(pong *p, int32_t up, int32_t down) {
  p->up = p->decay * p->up + up;
  p->down = p->decay * p->down + down;
  // The paddle.
  double big = p->up > p->down ? p->up : p->down;
  double gap = p->up - p->down;
  if (big > 0 && (gap < 0 ? -gap : gap) > DEAD_ZONE * big) {
    double step = PADDLE_SPEED * DT * (p->down > p->up ? 1 : -1);
    double at = p->paddle + step;
    p->paddle = at < PADDLE / 2 ? PADDLE / 2 : at > 1 - PADDLE / 2 ? 1 - PADDLE / 2 : at;
  }
  if (p->cond == 0 && p->t >= p->judged_at + MOVE_S - 1e-9) {
    double moved = p->paddle - p->judged_paddle;
    if (moved > 1e-9 || moved < -1e-9) {
      int towards = (moved > 0) == (p->y > p->judged_paddle);
      p->moves_judged++;
      p->moves_towards += towards;
      outcome(p, towards ? 1.0 : 0.0, 1);
    }
    p->judged_at = p->t;
    p->judged_paddle = p->paddle;
  }
  // The ball.
  p->x += p->vx * DT;
  p->y += p->vy * DT;
  if (p->y < 0 || p->y > 1) {
    p->y = p->y < 0 ? -p->y : 2 - p->y;
    p->vy = -p->vy;
  }
  if (p->x > 1) {
    p->x = 2 - p->x;
    p->vx = -p->vx;
  }
  if (p->x < 0) {
    double off = p->y - p->paddle;
    if ((off < 0 ? -off : off) <= PADDLE / 2) {
      p->x = -p->x;
      p->vx = -p->vx;
      aim(p);
      p->hits++;
      if (p->cond == 1)
        outcome(p, 1.0, 0);
    } else {
      p->rallies++;
      p->last_hits = p->hits;
      p->hits_all += p->hits;
      if (p->start >= EARLY_S) {
        p->late_rallies++;
        p->late_hits += p->hits;
      }
      if (p->cond == 1)
        outcome(p, 0.0, 0);
      p->t += DT;
      serve(p);
      return;
    }
  }
  // The ball's place and distance, as the sensory stims.
  if (p->t >= p->next_sense) {
    int32_t row;
    if (p->ego) {
      row = (int32_t)((p->y - p->paddle + 1) * 4);
      row = row < 0 ? 0 : row > 7 ? 7 : row;
    } else {
      row = (int32_t)(p->y * 8);
      row = row > 7 ? 7 : row;
    }
    emit(p, SENSORY[row], 1.0f, 1);
    double near = p->x < 0 ? 0 : p->x > 1 ? 1 : p->x;
    p->next_sense = p->t + 1 / (4 + 36 * (1 - near));
  }
  p->t += DT;
}

int32_t pong_run(pong *p, int32_t ticks, int32_t *spikes, int32_t *stims) {
  for (int32_t k = 0; k < ticks; ++k) {
    // The stims of the tick before, then the dish's 5 ms.
    for (int32_t i = 0; i < p->n_out; ++i) {
      int dup = 0;
      for (int32_t j = 0; j < i; ++j)
        dup |= p->out_ch[j] == p->out_ch[i];
      if (!dup)
        dish_stimulate(p->d, p->out_ch[i], p->out_amount[i]);
      if (stims && stims[p->out_ch[i]] < p->out_kind[i])
        stims[p->out_ch[i]] = p->out_kind[i];
    }
    p->n_out = 0;
    if (dish_step(p->d, STEPS) < 0)
      return -1;
    int32_t up = 0, down = 0;
    int64_t n;
    while ((n = dish_spikes(p->d, p->channel, p->step, 4096)) > 0)
      for (int64_t i = 0; i < n; ++i) {
        int32_t c = p->channel[i], column = c % 8;
        if (spikes)
          spikes[c]++;
        if (column <= 1)
          up++;
        else if (column >= 6)
          down++;
      }
    game_tick(p, up, down);
  }
  return 0;
}

void pong_state(pong *p, double *s) {
  s[0] = p->t;
  s[1] = p->x;
  s[2] = p->y;
  s[3] = p->paddle;
  s[4] = p->rallies;
  s[5] = p->hits;
  s[6] = p->last_hits;
  s[7] = (double)p->hits_all;
  s[8] = p->moves_judged;
  s[9] = p->moves_towards;
  s[10] = (double)p->late_rallies;
  s[11] = (double)p->late_hits;
}
