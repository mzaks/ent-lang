// Cars on ring roads of several lanes in plain C, as
// examples/traffic/lanes.ent drives them: the intelligent driver model
// behind the car ahead in the lane and MOBIL to change lanes, on the same
// roads, with the same f32 operations in the same order and the cars put
// in order by lane and place every step as there. Its output is that of
// test/Integration/Inputs/traffic_lanes.ent, to the bit: every 100 steps
// a line for each car, `step car lane x v` (the bits of the floats), and
// at the end each road's distance driven (the bits of the double), lane
// changes and steps standing on the car ahead.
//
//   cc -O2 -ffp-contract=off examples/traffic/check/lanes.c -lm -o lanes
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  float ring;
  int lanes, cars;
  float length, v0, t, s0, a, b, polite, threshold, safe, kick;
  int trucks;
  float truck_v0, truck_length, spread;
} plan;

// The roads of traffic_lanes.ent.
static const plan roads[] = {
    {1000.0f, 2, 40, 5.0f, 33.3f, 1.5f, 2.0f, 1.0f, 1.5f, 0.2f, 0.1f, 4.0f, 1.0f, 5, 22.2f, 12.0f, 0.3f},
    {1000.0f, 3, 30, 5.0f, 33.3f, 1.5f, 2.0f, 1.0f, 1.5f, 0.2f, 0.1f, 4.0f, 1.0f, 5, 22.2f, 12.0f, 0.3f},
    {600.0f, 3, 75, 5.0f, 30.0f, 1.2f, 2.0f, 0.8f, 1.5f, 0.5f, 0.2f, 3.0f, 2.0f, 4, 22.2f, 10.0f, 0.4f},
};
enum { ROADS = sizeof roads / sizeof roads[0], MOST = 256, STEPS = 6000, WARM = 3000 };
static const float dt = 0.1f;
static const uint64_t seed = 1;

static int cars;
static int road_of[MOST], lanes[MOST], base[MOST], lane[MOST], next_lane[MOST];
static int64_t key[MOST];
static float length[MOST], ring[MOST], v0[MOST], tt[MOST], s0[MOST], aa[MOST], sq[MOST];
static float polite[MOST], threshold[MOST], safe[MOST];
static float x[MOST], v[MOST], acc[MOST];
static double driven[ROADS];
static int changes[ROADS], crashes[ROADS];

// The order: the car in each row, as `Order` has them, and each car's row.
static int order[MOST], row_of[MOST], rows;

static uint64_t wymix(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)a * b;
  return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static uint64_t wyhash64(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)(a ^ 0x2d358dccaa6c78a5ull) * (b ^ 0x8bb84b93962eacc9ull);
  return wymix((uint64_t)r ^ 0x2d358dccaa6c78a5ull, (uint64_t)(r >> 64) ^ 0x8bb84b93962eacc9ull);
}

static double wyrand_unit(uint64_t k, int64_t n) {
  uint64_t s = k + ((uint64_t)n + 1) * 0x2d358dccaa6c78a5ull;
  return (double)(wymix(s, s ^ 0x8bb84b93962eacc9ull) >> 11) / 9007199254740992.0;
}

static int64_t key_of(int strip, float at) { return (int64_t)strip << 32 | (int64_t)(at * 1000.0f); }

static uint32_t bits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}

static uint64_t bits64(double f) {
  uint64_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}

// By key, and cars of one key in the order they were made (as `by` has them).
static int by_key(const void *p, const void *q) {
  int a = *(const int *)p, b = *(const int *)q;
  if (key[a] != key[b]) return key[a] < key[b] ? -1 : 1;
  return a - b;
}

static void sort(void) {
  rows = cars;
  for (int c = 0; c < cars; c++) order[c] = c;
  qsort(order, rows, sizeof order[0], by_key);
  for (int r = 0; r < rows; r++) row_of[order[r]] = r;
}

static int strip_of(int r) { return (int)(key[order[r]] >> 32); }

static int first(int64_t k, int above) {
  int low = 0, high = rows;
  while (low < high) {
    int mid = (low + high) >> 1;
    int64_t m = key[order[mid]];
    if (m < k || (above && m == k)) low = mid + 1; else high = mid;
  }
  return low;
}

static int ahead(int strip, int64_t k) {
  int r = first(k, 0);
  if (r < rows && strip_of(r) == strip) return r;
  int f = first((int64_t)strip << 32, 0);
  return f < rows && strip_of(f) == strip ? f : -1;
}

static int behind(int strip, int64_t k) {
  int r = first(k, 0) - 1;
  if (r >= 0 && strip_of(r) == strip) return r;
  int l = first((int64_t)(strip + 1) << 32, 0) - 1;
  return l >= 0 && strip_of(l) == strip ? l : -1;
}

static int next_in(int row, int strip) {
  if (row + 1 < rows && strip_of(row + 1) == strip) return row + 1;
  return first((int64_t)strip << 32, 0);
}

static int before_in(int row, int strip) {
  if (row > 0 && strip_of(row - 1) == strip) return row - 1;
  return first((int64_t)(strip + 1) << 32, 0) - 1;
}

static float reach(int r, int64_t k, int above, float at, float around) {
  float dx = x[order[r]] - at;
  if (key[order[r]] < k || (above && key[order[r]] == k)) dx += around;
  return dx;
}

static float reach_back(int r, int64_t k, float at, float around) {
  float dx = at - x[order[r]];
  if (key[order[r]] >= k) dx += around;
  return dx;
}

static float idm(int c, float speed, float front, float gap) {
  float r = speed / v0[c];
  float r2 = r * r;
  float y = speed * tt[c] + speed * (speed - front) / sq[c];
  float want = s0[c] + (y > 0.0f ? y : 0.0f);
  float q = want / gap;
  return aa[c] * (1.0f - r2 * r2 - q * q);
}

static void think(int tally, int counts) {
  for (int c = 0; c < cars; c++) {
    int r = next_in(row_of[c], base[c] + lane[c]);
    float gap = reach(r, key[c], 1, x[c], ring[c]) - length[order[r]];
    if (tally && counts && gap <= 0.0f) crashes[road_of[c]]++;
    acc[c] = idm(c, v[c], v[order[r]], gap);
  }
}

static void decide(int step) {
  int side = step % 2 == 0 ? 1 : -1;
  for (int c = 0; c < cars; c++) {
    next_lane[c] = lane[c];
    int to = lane[c] + side;
    if (to < 0 || to >= lanes[c]) continue;
    int own = base[c] + lane[c], there = base[c] + to;
    float gap = ring[c] - length[c];
    float front = v[c];
    int64_t k = key_of(there, x[c]);
    int nl = ahead(there, k);
    if (nl >= 0) {
      gap = reach(nl, k, 0, x[c], ring[c]) - length[order[nl]];
      front = v[order[nl]];
    }
    float mine = idm(c, v[c], front, gap);
    int ok = gap > 0.0f;
    float gain = mine - acc[c];
    int nf = behind(there, k);
    if (nf >= 0) {
      int who = order[nf];
      float back = reach_back(nf, k, x[c], ring[c]) - length[c];
      float then = idm(who, v[who], v[c], back);
      ok = ok && back > 0.0f && then >= 0.0f - safe[c];
      gain += polite[c] * (then - acc[who]);
    }
    int of = before_in(row_of[c], own);
    if (of >= 0 && order[of] != c) {
      int who = order[of];
      int ol = next_in(row_of[c], own);
      float back = reach(ol, key[who], 1, x[who], ring[c]) - length[order[ol]];
      float then = idm(who, v[who], v[order[ol]], back);
      gain += polite[c] * (then - acc[who]);
    }
    if (ok && gain > threshold[c]) next_lane[c] = to;
  }
}

int main(void) {
  int strips = 0;
  for (int i = 0; i < ROADS; i++) {
    const plan *p = &roads[i];
    float q = 2.0f * sqrtf(p->a * p->b);
    uint64_t k = wyhash64(seed, (uint64_t)i);
    for (int n = 0; n < p->cars; n++, cars++) {
      float at = (float)n * p->ring / (float)p->cars;
      if (n == 0) at += p->kick;
      int truck = p->trucks > 0 && n % p->trucks == p->trucks - 1;
      float wish = truck ? p->truck_v0 : p->v0;
      float draw = (float)wyrand_unit(k, n);
      road_of[cars] = i;
      lanes[cars] = p->lanes;
      base[cars] = strips;
      lane[cars] = n % p->lanes;
      length[cars] = truck ? p->truck_length : p->length;
      ring[cars] = p->ring;
      v0[cars] = wish * (1.0f + p->spread * (draw - 0.5f));
      tt[cars] = p->t;
      s0[cars] = p->s0;
      aa[cars] = p->a;
      sq[cars] = q;
      polite[cars] = p->polite;
      threshold[cars] = p->threshold;
      safe[cars] = p->safe;
      x[cars] = at;
      key[cars] = key_of(strips + lane[cars], at);
    }
    strips += p->lanes;
  }
  for (int step = 0; step < STEPS; step++) {
    int counts = step >= WARM;
    if (step % 100 == 0)
      for (int c = 0; c < cars; c++)
        printf("%d %d %d %u %u\n", step, c, lane[c], bits(x[c]), bits(v[c]));
    sort();
    think(0, counts);
    decide(step);
    for (int c = 0; c < cars; c++)
      if (next_lane[c] != lane[c]) {
        lane[c] = next_lane[c];
        key[c] = key_of(base[c] + lane[c], x[c]);
        if (counts) changes[road_of[c]]++;
      }
    sort();
    think(1, counts);
    for (int c = 0; c < cars; c++) {
      float a = acc[c];
      float next = v[c] + a * dt;
      float dx;
      if (next < 0.0f) {
        dx = 0.0f - 0.5f * v[c] * v[c] / a;
        v[c] = 0.0f;
      } else {
        dx = v[c] * dt + 0.5f * a * dt * dt;
        v[c] = next;
      }
      float at = x[c] + dx;
      if (at >= ring[c]) at -= ring[c];
      x[c] = at;
      key[c] = key_of(base[c] + lane[c], at);
      if (counts) driven[road_of[c]] += (double)dx;
    }
  }
  for (int i = 0; i < ROADS; i++)
    printf("road of %d lanes %d cars driven %llu changes %d crashes %d\n", roads[i].lanes, roads[i].cars,
           (unsigned long long)bits64(driven[i]), changes[i], crashes[i]);
  return 0;
}
