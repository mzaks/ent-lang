// The intelligent driver model on ring roads in plain C, as
// examples/traffic/idm.ent drives it: the same roads, the same steps, the
// same f32 operations in the same order. Its output is that of
// test/Integration/Inputs/traffic_idm.ent, to the bit: every 100 steps a
// line for each car, `step car x v` (the bits of the floats), and at the
// end each road's (by its cars) distance driven (the bits of the double) and stops.
//
//   cc -O2 -ffp-contract=off examples/traffic/check/idm.c -lm -o idm
//
// (-ffp-contract=off: a multiply and an add stay two roundings, as they
// are in ent.)
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  float ring;
  int cars;
  float length, v0, t, s0, a, b, kick;
} plan;

// The roads of traffic_idm.ent.
static const plan roads[] = {
    {230.0f, 22, 5.0f, 15.0f, 1.0f, 2.0f, 1.0f, 1.5f, 0.5f},
    {400.0f, 30, 4.5f, 20.0f, 1.2f, 1.5f, 0.8f, 2.0f, 1.0f},
};
enum { ROADS = sizeof roads / sizeof roads[0], MOST = 64, STEPS = 6000, WARM = 3000 };
static const float dt = 0.1f;

static int cars;
static int road_of[MOST], lead[MOST];
static float length[MOST], ring[MOST], v0[MOST], tt[MOST], s0[MOST], aa[MOST], bb[MOST], sq[MOST];
static float x[MOST], v[MOST], acc[MOST];
static int moving[MOST];
static double driven[ROADS];
static int stops[ROADS];

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

int main(void) {
  for (int i = 0; i < ROADS; i++) {
    const plan *p = &roads[i];
    float q = 2.0f * sqrtf(p->a * p->b);
    int first = cars;
    for (int k = 0; k < p->cars; k++) {
      float at = (float)k * p->ring / (float)p->cars;
      if (k == 0) at += p->kick;
      road_of[cars] = i;
      length[cars] = p->length;
      ring[cars] = p->ring;
      v0[cars] = p->v0;
      tt[cars] = p->t;
      s0[cars] = p->s0;
      aa[cars] = p->a;
      bb[cars] = p->b;
      sq[cars] = q;
      x[cars] = at;
      lead[cars] = k + 1 < p->cars ? cars + 1 : first;
      cars++;
    }
  }
  for (int step = 0; step < STEPS; step++) {
    if (step % 100 == 0)
      for (int c = 0; c < cars; c++)
        printf("%d %d %u %u\n", step, c, bits(x[c]), bits(v[c]));
    for (int c = 0; c < cars; c++) {
      int l = lead[c];
      float dx = x[l] - x[c];
      if (dx <= 0.0f) dx += ring[c];
      float gap = dx - length[c];
      float r = v[c] / v0[c];
      float r2 = r * r;
      float y = v[c] * tt[c] + v[c] * (v[c] - v[l]) / sq[c];
      float want = s0[c] + (y > 0.0f ? y : 0.0f);
      float q = want / gap;
      acc[c] = aa[c] * (1.0f - r2 * r2 - q * q);
    }
    int counts = step >= WARM;
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
      if (v[c] > 3.0f) moving[c] = 1;
      if (moving[c] && v[c] < 1.0f) {
        moving[c] = 0;
        if (counts) stops[road_of[c]]++;
      }
      if (counts) driven[road_of[c]] += (double)dx;
    }
  }
  for (int i = 0; i < ROADS; i++)
    printf("road of %d cars driven %llu stops %d\n", roads[i].cars, (unsigned long long)bits64(driven[i]), stops[i]);
  return 0;
}
