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
//       relative to the paddle (pong.py's BALL_EGO). cond 2-4 are
//       DishBrain's feedback on a dish with plain STDP (no dopamine; its
//       reward and punish channels are electrodes like the others):
//       2 `stim` (a hit: the 8 sensory electrodes at 100 Hz for 100 ms; a
//       miss: 4 s of stims on random electrodes at 5 Hz, a pause), 3
//       `silent` (a miss: 4 s with no input), 4 `none`, as pong.py's conditions of those names
//   int32_t pong_run(p, ticks, spikes, stims)  runs `ticks` ticks; adds
//       each channel's spikes to spikes[64]; stims[64] the most of each
//       channel's stims (1 a sensory stim, 2 a reward, 3 a punish pulse)
//   void pong_zones(p, int32_t z[16])   the balls that reached the paddle's
//       line in each eighth of its height (z[0..7], 0 the top) and of
//       those the paddle hit (z[8..15])
//   void pong_rows(p, int32_t r[24])   by the row of the ball's sensory
//       stims (the court's or, ego, against the paddle): the stims (r[0..7]),
//       the moves judged with that row the last stim (r[8..15]) and of
//       those the moves towards the ball (r[16..23])
//   void pong_decoder(p, d)   how the paddle reads the motor regions: 0 (the
//       default) their spikes as they come; 1 each against its own
//       background (a 2 s average), so that a region that only fires more
//       does not pull; 2 only the spikes of the 30 ms after a ball stim
//   void pong_oracle(p, o)   the paddle steered by the host, not the dish
//       (which still runs): 1 towards the ball's height now, 2 to where it
//       will cross the paddle's line, 3 by the last sensory stim's row
//       alone (ego: up for rows 0-3, down for 4-7), what the code tells;
//       each at the paddle's speed. 0 the dish (the default)
//   void pong_motor(p, m)   which electrodes the motor regions are: 0 (the
//       default) columns 0-1 up and 6-7 down, 1 only the inner ones (1 and
//       6, nearer the sensory electrodes), 2 only the outer (0 and 7)
//   void pong_layout(aligned)   for the dishes and games made after it
//       (process-wide): 1 the sensory rows on the columns nearer the way
//       each wants (zones.py layouts), 2 the same with two electrodes a
//       row (all of columns 3 and 4), 0 as before
//   void pong_far_serves(p, share)   from the next serve on, that share of
//       the serves from the half of the court the paddle is not in, and
//       flat, a quarter of the usual slope, so that they stay there
//       (practice where the ball is far from it; 0: off, the default)
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
#define BASE_TAU 2.0           // decoder 1: a region's background, s
#define EVOKED_S 0.03          // decoder 2: the motor spikes this long after a ball stim
#define DEAD_ZONE 0.1
#define RPE_RATE 0.05
#define MOVE_S 0.1
#define EARLY_S 300.0
#define REWARD 28
#define PUNISH 36
#define HIT_HZ 100.0           // cond 2: the predictable feedback
#define HIT_S 0.1
#define MISS_HZ 5.0            // and the unpredictable one, and the pause
#define MISS_S 4.0
#define QUARTER_PI 0.78539816339744830962

static const int32_t SENSORY[8] = {3, 12, 20, 27, 35, 44, 52, 59};
// pong_layout(1): the top four rows on column 3, nearer the up regions,
// the bottom four on column 4, nearer the down ones; the punish channel
// (dopamine for the whole dish, wherever it is) moves to the 35 that
// frees, as 36 is a row's.
static const int32_t SENSORY_ALIGNED[8] = {3, 11, 19, 27, 36, 44, 52, 60};
// pong_layout(2): as 1 with two electrodes a row, all of columns 3 (rows
// 0-3, two grid rows each) and 4 (rows 4-7): as far from the motor
// regions, twice the synapses; reward and punish on 26 and 37.
static const int32_t SENSORY_TWO[8] = {3, 19, 35, 51, 4, 20, 36, 52};   // and 8 more
static const int32_t SENSORY_TWO_B[8] = {11, 27, 43, 59, 12, 28, 44, 60};
static const int32_t *sensory = SENSORY;
static const int32_t *sensory_b = 0;
static int32_t punish = PUNISH, reward = REWARD;

void pong_layout(int32_t aligned) {
  sensory = aligned == 2 ? SENSORY_TWO : aligned ? SENSORY_ALIGNED : SENSORY;
  sensory_b = aligned == 2 ? SENSORY_TWO_B : 0;
  punish = aligned == 2 ? 37 : aligned ? 35 : PUNISH;
  reward = aligned == 2 ? 26 : REWARD;
}

typedef struct pong {
  dish *d;
  uint64_t rng, fixed;
  double far;
  int32_t decoder, motor, oracle;
  double base_up, base_down, base_decay, last_sense;
  int32_t cond, ego;
  double t, x, y, vx, vy, paddle, up, down, decay;
  double next_sense, judged_at, judged_paddle, expect, expect_move, start;
  int32_t hits, rallies, last_hits, moves_judged, moves_towards;
  int64_t hits_all, late_rallies, late_hits;
  int32_t zone_seen[8], zone_hit[8];
  int32_t last_row, row_sensed[8], row_judged[8], row_towards[8];
  int32_t channel[4096], step[4096];
  // This tick's stims: channel, amount, kind.
  int32_t n_out, out_ch[16], out_kind[16];
  float out_amount[16];
  // DishBrain's feedback (cond 2, 3): the pulses still to come.
  uint64_t fb_rng;
  int32_t hit_left, miss_left;
  double hit_next, miss_next, paused_until;
} pong;

// (splitmix64: the same numbers everywhere.)
static double unit_of(uint64_t *state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double next_unit(pong *p) { return unit_of(&p->rng); }

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
  int far = p->far > 0 && next_unit(p) < p->far;
  if (far)
    p->y = p->paddle < 0.5 ? uniform(p, 0.6, 0.9) : uniform(p, 0.1, 0.4);
  p->vx = -BALL_SPEED;
  aim(p);
  if (far)
    p->vy *= 0.25;   // flat: it stays in that half on its way
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
    emit(p, err > 0 ? reward : punish, (float)(err > 0 ? err : -err), err > 0 ? 2 : 3);
}

dish *pong_make_dish(int64_t seed, double gain, int32_t cond) {
  dish_params *params = malloc((size_t)dish_params_size());
  if (!params)
    return NULL;
  dish_defaults(params);
  dish_params_set(params, "seed", (double)seed);
  dish_params_set(params, "axon", 0.11);
  dish_params_set(params, "hard", 1);
  dish_params_set(params, "modulated", cond < 2);
  dish_params_set(params, "reward_channel", cond < 2 ? reward : -1);
  dish_params_set(params, "punish_channel", cond < 2 ? punish : -1);
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
  p->last_row = -1;
  p->rng = (uint64_t)seed * 7919u + 1u;
  p->cond = cond;
  p->ego = ego;
  p->fb_rng = (uint64_t)seed * 104729u + 7u;
  p->paddle = 0.5;
  p->expect = p->expect_move = 0.5;
  p->decay = exp_small(-DT / MOTOR_TAU);
  serve(p);
  return p;
}

void pong_destroy(pong *p) { free(p); }

void pong_fixed_serves(pong *p, int64_t base) { p->fixed = (uint64_t)base; }

void pong_far_serves(pong *p, double share) { p->far = share; }

void pong_motor(pong *p, int32_t m) { p->motor = m; }

void pong_oracle(pong *p, int32_t o) { p->oracle = o; }

// Where the ball will cross the paddle's line, with its bounces.
static double arrival(const pong *p) {
  double speed = p->vx < 0 ? -p->vx : p->vx;
  double t = p->vx < 0 ? p->x / speed : (2 - p->x) / speed;
  double y = p->y + p->vy * t;
  y -= 2.0 * (double)(int64_t)(y / 2.0);
  if (y < 0)
    y += 2;
  return y > 1 ? 2 - y : y;
}

void pong_decoder(pong *p, int32_t decoder) {
  p->decoder = decoder;
  p->base_decay = exp_small(-DT / BASE_TAU);
}

void pong_zones(pong *p, int32_t *z) {
  for (int k = 0; k < 8; ++k) {
    z[k] = p->zone_seen[k];
    z[8 + k] = p->zone_hit[k];
  }
}

void pong_rows(pong *p, int32_t *r) {
  for (int k = 0; k < 8; ++k) {
    r[k] = p->row_sensed[k];
    r[8 + k] = p->row_judged[k];
    r[16 + k] = p->row_towards[k];
  }
}

// One tick of the game, with the spikes of the motor regions this tick.
static void game_tick(pong *p, int32_t up, int32_t down) {
  while (p->hit_left > 0 && p->t >= p->hit_next - 1e-9) {
    for (int k = 0; k < 8; ++k)
      emit(p, sensory[k], 1.0f, 1);
    p->hit_left--;
    p->hit_next += 1.0 / HIT_HZ;
  }
  while (p->miss_left > 0 && p->t >= p->miss_next - 1e-9) {
    int32_t c = (int32_t)(unit_of(&p->fb_rng) * 64);
    emit(p, c > 63 ? 63 : c, 1.0f, 1);
    p->miss_left--;
    p->miss_next += 1.0 / MISS_HZ;
  }
  if (p->decoder == 2 && p->t - p->last_sense > EVOKED_S) // only what a stim evoked
    up = down = 0;
  p->up = p->decay * p->up + up;
  p->down = p->decay * p->down + down;
  if (p->t < p->paused_until) {
    p->t += DT;
    return;
  }
  // The paddle.
  double big = p->up > p->down ? p->up : p->down;
  double gap = p->up - p->down;
  if (p->decoder == 1) {   // each region against its own background
    p->base_up = p->base_decay * p->base_up + (1 - p->base_decay) * p->up;
    p->base_down = p->base_decay * p->base_down + (1 - p->base_decay) * p->down;
    gap = (p->up - p->base_up) - (p->down - p->base_down);
  }
  if (p->oracle) {
    double want = 0;
    if (p->oracle == 3) {
      want = p->last_row < 0 ? 0 : p->last_row < 4 ? -1 : 1;
    } else {
      double target = p->oracle == 2 ? arrival(p) : p->y;
      double off = target - p->paddle;
      want = off > 0.02 ? 1 : off < -0.02 ? -1 : 0;
    }
    double at = p->paddle + PADDLE_SPEED * DT * want;
    p->paddle = at < PADDLE / 2 ? PADDLE / 2 : at > 1 - PADDLE / 2 ? 1 - PADDLE / 2 : at;
  } else if (big > 0 && (gap < 0 ? -gap : gap) > DEAD_ZONE * big) {
    double step = PADDLE_SPEED * DT * (gap < 0 ? 1 : -1);
    double at = p->paddle + step;
    p->paddle = at < PADDLE / 2 ? PADDLE / 2 : at > 1 - PADDLE / 2 ? 1 - PADDLE / 2 : at;
  }
  if (p->cond == 0 && p->t >= p->judged_at + MOVE_S - 1e-9) {
    double moved = p->paddle - p->judged_paddle;
    if (moved > 1e-9 || moved < -1e-9) {
      int towards = (moved > 0) == (p->y > p->judged_paddle);
      p->moves_judged++;
      p->moves_towards += towards;
      if (p->last_row >= 0) {
        p->row_judged[p->last_row]++;
        p->row_towards[p->last_row] += towards;
      }
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
    int32_t zone = (int32_t)(p->y * 8);
    zone = zone < 0 ? 0 : zone > 7 ? 7 : zone;
    p->zone_seen[zone]++;
    if ((off < 0 ? -off : off) <= PADDLE / 2)
      p->zone_hit[zone]++;
    if ((off < 0 ? -off : off) <= PADDLE / 2) {
      p->x = -p->x;
      p->vx = -p->vx;
      aim(p);
      p->hits++;
      if (p->cond == 2) {
        p->hit_left = (int32_t)(HIT_HZ * HIT_S + 0.5);
        p->hit_next = p->t;
      }
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
      if (p->cond == 2) {
        p->miss_left = (int32_t)(MISS_HZ * MISS_S + 0.5);
        p->miss_next = p->t;
      }
      if (p->cond == 2 || p->cond == 3)
        p->paused_until = p->t + MISS_S;
      p->t += DT;
      serve(p);
      if (p->paused_until > p->t)
        p->start = p->paused_until;
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
    emit(p, sensory[row], 1.0f, 1);
    if (sensory_b)
      emit(p, sensory_b[row], 1.0f, 1);
    p->last_row = row;
    p->last_sense = p->t;
    p->row_sensed[row]++;
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
        int lo = p->motor == 2 ? 0 : p->motor == 1 ? 1 : 0;
        int hi = p->motor == 1 ? 1 : p->motor == 2 ? 0 : 1;
        if (column >= lo && column <= hi)
          up++;
        else if (column >= 7 - hi && column <= 7 - lo)
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
