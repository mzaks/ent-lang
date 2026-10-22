// The LJ melt of examples/lj/lj.ent in plain C: the same lattice, the
// same velocities, the same steps, and the same lists of neighbours,
// made every 20 steps of the atoms within the cut and the skin as the
// grid index `Cells` finds them, in the same order (the cells around an
// atom's, z, then y, then x, each cell's atoms in the index's order: put
// in order by cell once by insertion from the order they were made in,
// and again by insertion from the last order each time), the atoms put
// back in the box first (only then: between, they leave it), and those
// seen across the side of the box in a list of their own, of which the
// force takes the nearest image, after the others. Of half lists,
// the force an atom gives the other of a pair is added in the order of
// the atoms that give it. So the sums are the same to the bit. Its output
// is that of test/Integration/Inputs/lj_melt.ent (half lists) and
// lj_melt_full.ent: every `print` steps the bits of every atom's place
// and velocity, and of the sums.
//
//   cc -O2 -ffp-contract=off examples/lj/check/melt.c -lm -o melt
//   ./melt cells steps print half
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAPACITY = 32768 };
static const uint64_t seed = 87287;
static const double density = 0.8442, temperature = 1.44, cut = 2.5,
                    skin = 0.3, dt = 0.005;
enum { EVERY = 20, MOST = 4194304 };
static int half;

static int atoms;
static double x[CAPACITY], y[CAPACITY], z[CAPACITY];
static double vx[CAPACITY], vy[CAPACITY], vz[CAPACITY];
static double fx[CAPACITY], fy[CAPACITY], fz[CAPACITY], fe[CAPACITY],
    fw[CAPACITY];
// The force given by the atoms whose half lists an atom is in.
static double px_[CAPACITY], py_[CAPACITY], pz_[CAPACITY];
// The lists: where each starts and how long it is, one after another;
// and those of the pairs seen across the side of the box.
static int first_[CAPACITY], count_[CAPACITY];
static int pairs_[MOST], rows;
static int cfirst_[CAPACITY], ccount_[CAPACITY];
static int cross_[MOST], crows;
static int64_t pairs[CAPACITY];
static double size;

// The index: each atom's cell, the atoms in order by cell, a copy of
// where each was when it was made, and each cell's first row.
static int64_t key[CAPACITY];
static int order[CAPACITY], row_of[CAPACITY];
static double rx[CAPACITY], ry[CAPACITY], rz[CAPACITY];
static int first[CAPACITY + 1];
static int64_t n;
static double w;

static uint64_t wymix(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)a * b;
  return (uint64_t)(r >> 64) ^ (uint64_t)r;
}

static uint64_t wyhash64(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)(a ^ 0x2d358dccaa6c78a5ull) * (b ^ 0x8bb84b93962eacc9ull);
  return wymix((uint64_t)r ^ 0x2d358dccaa6c78a5ull, (uint64_t)(r >> 64) ^ 0x8bb84b93962eacc9ull);
}

static double wyrand_unit(uint64_t k, int64_t i) {
  uint64_t s = k + ((uint64_t)i + 1) * 0x2d358dccaa6c78a5ull;
  return (double)(wymix(s, s ^ 0x8bb84b93962eacc9ull) >> 11) / 9007199254740992.0;
}

static double cube_root(double v) {
  double r = 1.0;
  for (int i = 0; i < 60; i++) r = r - (r * r * r - v) / (3.0 * r * r);
  return r;
}

static int64_t cell_of(double p) {
  double q = (p - 0.0) / w;
  int64_t c = (int64_t)q;
  if ((double)c > q) c -= 1;
  c = c % n;
  if (c < 0) c += n;
  return c;
}

static int64_t at(int64_t k) {
  int64_t c = k % n;
  if (c < 0) c += n;
  return c;
}

static int across(double ax, double ay, double az) {
  return ax <= 0.0 || ax >= size || ay <= 0.0 || ay >= size || az <= 0.0 ||
         az >= size;
}

static double image(double p, double from) {
  double d = p - from;
  if (d > size * 0.5) return p - size;
  if (d < 0.0 - size * 0.5) return p + size;
  return p;
}

static void make_index(void) {
  double reach = cut + skin;
  n = (int64_t)(size / reach);
  if (n < 1) n = 1;
  while (n * n * n > CAPACITY) {
    n = n / 2 < 1 ? 1 : n / 2;
  }
  w = size / (double)n;
  for (int i = 0; i < atoms; i++)
    key[i] = (cell_of(z[i]) * n + cell_of(y[i])) * n + cell_of(x[i]);
  for (int i = 1; i < atoms; i++) {
    int a = order[i];
    int j = i;
    while (j > 0 && key[order[j - 1]] > key[a]) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = a;
  }
  for (int r = 0; r < atoms; r++) {
    row_of[order[r]] = r;
    rx[r] = x[order[r]];
    ry[r] = y[order[r]];
    rz[r] = z[order[r]];
  }
  int r = 0;
  for (int64_t c = 0; c < n * n * n + 1; c++) {
    while (r < atoms && key[order[r]] < c) r++;
    first[c] = r;
  }
}

// Each atom's list (of a half list, only the atoms after it in the
// index's order, as `after me` has them), counted and then filled: here
// in one go, in the same order.
static void make_lists(void) {
  for (int i = 0; i < atoms; i++) {
    if (x[i] >= size) x[i] -= size;
    if (x[i] < 0.0) x[i] += size;
    if (y[i] >= size) y[i] -= size;
    if (y[i] < 0.0) y[i] += size;
    if (z[i] >= size) z[i] -= size;
    if (z[i] < 0.0) z[i] += size;
  }
  make_index();
  double reach = cut + skin;
  int64_t m = (int64_t)(reach / w);
  if ((double)m * w < reach) m += 1;
  int64_t many = 2 * m + 1 < n ? 2 * m + 1 : n;
  double far = reach * reach;
  rows = 0;
  crows = 0;
  for (int i = 0; i < atoms; i++) {
    first_[i] = rows;
    cfirst_[i] = crows;
    int64_t own[3] = {cell_of(x[i]), cell_of(y[i]), cell_of(z[i])};
    int64_t from[3];
    for (int d = 0; d < 3; d++) from[d] = 2 * m + 1 >= n ? 0 : own[d] - m;
    for (int64_t kz = 0; kz < many; kz++) {
      int64_t cz = at(from[2] + kz);
      for (int64_t ky = 0; ky < many; ky++) {
        int64_t cy = at(from[1] + ky);
        for (int64_t kx = 0; kx < many; kx++) {
          int64_t cx = at(from[0] + kx);
          int64_t c = (cz * n + cy) * n + cx;
          for (int r = first[c]; r < first[c + 1]; r++) {
            double sx = image(rx[r], x[i]), sy = image(ry[r], y[i]),
                   sz = image(rz[r], z[i]);
            double ax = sx - x[i], ay = sy - y[i], az = sz - z[i];
            if (!(ax * ax + ay * ay + az * az <= far)) continue;
            int j = order[r];
            if (j == i || (half && !(row_of[j] > row_of[i]))) continue;
            if (across(sx, sy, sz))
              cross_[crows++] = j;
            else
              pairs_[rows++] = j;
          }
        }
      }
    }
    count_[i] = rows - first_[i];
    ccount_[i] = crows - cfirst_[i];
  }
}

static void forces(void) {
  double far = cut * cut;
  if (half)
    for (int i = 0; i < atoms; i++) px_[i] = py_[i] = pz_[i] = 0.0;
  for (int i = 0; i < atoms; i++) {
    double sx = 0.0, sy = 0.0, sz = 0.0, e = 0.0, wv = 0.0;
    int64_t count = 0;
    for (int k = 0; k < count_[i]; k++) {
      int j = pairs_[first_[i] + k];
      double dx = x[j] - x[i];
      double dy = y[j] - y[i];
      double dz = z[j] - z[i];
      double r2 = dx * dx + dy * dy + dz * dz;
      if (!(r2 <= far)) continue;
      double r2i = 1.0 / r2;
      double r6i = r2i * r2i * r2i;
      double pair = 48.0 * r6i * (r6i - 0.5) * r2i;
      sx -= dx * pair;
      sy -= dy * pair;
      sz -= dz * pair;
      if (half) {
        px_[j] += dx * pair;
        py_[j] += dy * pair;
        pz_[j] += dz * pair;
        e += 4.0 * r6i * (r6i - 1.0);
        wv += pair * r2;
      } else {
        e += 2.0 * r6i * (r6i - 1.0);
        wv += 0.5 * pair * r2;
      }
      count += 1;
    }
    for (int k = 0; k < ccount_[i]; k++) {
      int j = cross_[cfirst_[i] + k];
      double dx = image(x[j], x[i]) - x[i];
      double dy = image(y[j], y[i]) - y[i];
      double dz = image(z[j], z[i]) - z[i];
      double r2 = dx * dx + dy * dy + dz * dz;
      if (!(r2 <= far)) continue;
      double r2i = 1.0 / r2;
      double r6i = r2i * r2i * r2i;
      double pair = 48.0 * r6i * (r6i - 0.5) * r2i;
      sx -= dx * pair;
      sy -= dy * pair;
      sz -= dz * pair;
      if (half) {
        px_[j] += dx * pair;
        py_[j] += dy * pair;
        pz_[j] += dz * pair;
        e += 4.0 * r6i * (r6i - 1.0);
        wv += pair * r2;
      } else {
        e += 2.0 * r6i * (r6i - 1.0);
        wv += 0.5 * pair * r2;
      }
      count += 1;
    }
    fx[i] = sx;
    fy[i] = sy;
    fz[i] = sz;
    fe[i] = e;
    fw[i] = wv;
    pairs[i] = count;
  }
}

static uint64_t bits(double f) {
  uint64_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}

int main(int argc, char **argv) {
  int cells = argc > 1 ? atoi(argv[1]) : 6;
  int steps = argc > 2 ? atoi(argv[2]) : 100;
  int print = argc > 3 ? atoi(argv[3]) : 20;
  half = argc > 4 ? atoi(argv[4]) : 1;
  double a = cube_root(4.0 / density);
  size = a * (double)cells;
  uint64_t k = wyhash64(seed, 1);
  for (int i = 0; i < cells; i++)
    for (int j = 0; j < cells; j++)
      for (int l = 0; l < cells; l++)
        for (int b = 0; b < 4; b++) {
          double bx = (b == 1 || b == 2) ? 0.5 : 0.0;
          double by = (b == 1 || b == 3) ? 0.5 : 0.0;
          double bz = (b == 2 || b == 3) ? 0.5 : 0.0;
          int64_t m = atoms;
          x[m] = ((double)i + bx) * a;
          y[m] = ((double)j + by) * a;
          z[m] = ((double)l + bz) * a;
          vx[m] = wyrand_unit(k, 3 * m) - 0.5;
          vy[m] = wyrand_unit(k, 3 * m + 1) - 0.5;
          vz[m] = wyrand_unit(k, 3 * m + 2) - 0.5;
          order[m] = (int)m;
          atoms++;
        }
  double px = 0.0, py = 0.0, pz = 0.0;
  for (int i = 0; i < atoms; i++) {
    px += vx[i];
    py += vy[i];
    pz += vz[i];
  }
  double mx = px / (double)atoms, my = py / (double)atoms,
         mz = pz / (double)atoms;
  for (int i = 0; i < atoms; i++) {
    vx[i] -= mx;
    vy[i] -= my;
    vz[i] -= mz;
  }
  double vv = 0.0;
  for (int i = 0; i < atoms; i++) vv += vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i];
  double dof = (double)(3 * (int64_t)atoms - 3);
  double by = sqrt(temperature / (vv / dof));
  for (int i = 0; i < atoms; i++) {
    vx[i] *= by;
    vy[i] *= by;
    vz[i] *= by;
  }
  make_lists();
  forces();
  for (int step = 0;; step++) {
    if (step % print == 0) {
      double svv = 0.0, spe = 0.0, sw = 0.0;
      int64_t sp = 0;
      for (int i = 0; i < atoms; i++) {
        svv += vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i];
        spe += fe[i];
        sw += fw[i];
        sp += pairs[i];
      }
      printf("step %d vv %llu pe %llu w %llu pairs %lld\n", step,
             (unsigned long long)bits(svv), (unsigned long long)bits(spe),
             (unsigned long long)bits(sw), (long long)sp);
      for (int i = 0; i < atoms; i++)
        printf("%d %llu %llu %llu %llu\n", i, (unsigned long long)bits(x[i]),
               (unsigned long long)bits(y[i]), (unsigned long long)bits(z[i]),
               (unsigned long long)bits(vx[i]));
    }
    if (step == steps) break;
    for (int i = 0; i < atoms; i++) {
      vx[i] += 0.5 * dt * (fx[i] + px_[i]);
      vy[i] += 0.5 * dt * (fy[i] + py_[i]);
      vz[i] += 0.5 * dt * (fz[i] + pz_[i]);
      x[i] += dt * vx[i];
      y[i] += dt * vy[i];
      z[i] += dt * vz[i];
    }
    if ((step + 1) % EVERY == 0) make_lists();
    forces();
    for (int i = 0; i < atoms; i++) {
      vx[i] += 0.5 * dt * (fx[i] + px_[i]);
      vy[i] += 0.5 * dt * (fy[i] + py_[i]);
      vz[i] += 0.5 * dt * (fz[i] + pz_[i]);
    }
  }
  return 0;
}
