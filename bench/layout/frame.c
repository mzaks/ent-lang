// The example's fused frame, written by hand in three memory layouts, to
// decide whether a layout pass is worth building before building it.
//
//   -DLAYOUT=0  SoA:   one array per field (what the compiler emits today)
//   -DLAYOUT=1  AoS:   one struct per entity
//   -DLAYOUT=2  AoSoA: blocks of W entities, one array per field per block
//   -DW=8       AoSoA block width
//   -DSTAGGER   SoA only: offset each column by 17 more cache lines, so
//               columns do not start at the same cache set
//   -fopenmp    split entities (or blocks) over threads
//
// Usage: frame <entities per archetype> <repetitions> <target ms>
// Prints: n=<N> frames=<F> ns_per_frame=<best> checksum=<sum>
//
// Every layout performs the same floating point operations per entity in
// the same order, so checksums must agree bit for bit.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma clang fp contract(off)

#ifndef LAYOUT
#define LAYOUT 0
#endif
#ifndef W
#define W 8
#endif

enum { FIELDS = 5, VALIDATION_FRAMES = 10 };
static const float G = 9.81f, WIND = 2.0f;

// Initial value of field f of entity i; the same for every layout.
static float initial(int archetype, int f, int64_t i) {
  static const float base[2][FIELDS] = {{0, 100, 1, 0, 1}, {0, 0, 2, 1, 10}};
  static const float step[2][FIELDS] = {{0.5f, 0.25f, 0.01f, 0.02f, 0.1f},
                                        {0.5f, 0.25f, -0.01f, 0.02f, 0.1f}};
  return base[archetype][f] + step[archetype][f] * (float)(i % 1024);
}

static void *allocate(size_t bytes) {
  void *p;
  if (posix_memalign(&p, 64, bytes) != 0) {
    perror("posix_memalign");
    exit(1);
  }
  return p;
}

// Fields: 0 x, 1 y, 2 dx, 3 dy, 4 kg (bodies) or life (particles).
#if LAYOUT == 0
typedef struct {
  float *f[FIELDS];
} Table;
static Table makeTable(int a, int64_t n) {
  Table t;
  for (int f = 0; f < FIELDS; ++f) {
#ifdef STAGGER
    // Large allocations come back page or 4 MB aligned, so every column
    // would start in the same cache set.
    t.f[f] = (float *)allocate((size_t)n * sizeof(float) + 64 * 17 * 16) +
             16 * 17 * (f + FIELDS * a);
#else
    t.f[f] = allocate((size_t)n * sizeof(float));
#endif
    for (int64_t i = 0; i < n; ++i)
      t.f[f][i] = initial(a, f, i);
  }
  return t;
}
static float get(Table *t, int f, int64_t i) { return t->f[f][i]; }

static void frame(Table *b, Table *p, int64_t n, float dt) {
  float *x = b->f[0], *y = b->f[1], *dx = b->f[2], *dy = b->f[3];
#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < n; ++i) {
    dy[i] = dy[i] - G * dt;
    x[i] = x[i] + dx[i] * dt;
    y[i] = y[i] + dy[i] * dt;
  }
  float *px = p->f[0], *py = p->f[1], *pdx = p->f[2], *pdy = p->f[3],
        *life = p->f[4];
#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < n; ++i) {
    pdx[i] = pdx[i] + WIND * dt;
    px[i] = px[i] + pdx[i] * dt;
    py[i] = py[i] + pdy[i] * dt;
    life[i] = life[i] - dt;
  }
}

#elif LAYOUT == 1
typedef struct {
  float x, y, dx, dy, last;
} Entity;
typedef struct {
  Entity *e;
} Table;
static Table makeTable(int a, int64_t n) {
  Table t = {allocate((size_t)n * sizeof(Entity))};
  for (int64_t i = 0; i < n; ++i) {
    float *fields = &t.e[i].x;
    for (int f = 0; f < FIELDS; ++f)
      fields[f] = initial(a, f, i);
  }
  return t;
}
static float get(Table *t, int f, int64_t i) { return (&t->e[i].x)[f]; }

static void frame(Table *b, Table *p, int64_t n, float dt) {
  Entity *e = b->e;
#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < n; ++i) {
    e[i].dy = e[i].dy - G * dt;
    e[i].x = e[i].x + e[i].dx * dt;
    e[i].y = e[i].y + e[i].dy * dt;
  }
  Entity *q = p->e;
#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < n; ++i) {
    q[i].dx = q[i].dx + WIND * dt;
    q[i].x = q[i].x + q[i].dx * dt;
    q[i].y = q[i].y + q[i].dy * dt;
    q[i].last = q[i].last - dt;
  }
}

#elif LAYOUT == 2
typedef struct {
  float x[W], y[W], dx[W], dy[W], last[W];
} Block;
typedef struct {
  Block *blocks;
} Table;
// Entities past n in the last block are padding: computed, never checked.
static int64_t numBlocks(int64_t n) { return (n + W - 1) / W; }
static Table makeTable(int a, int64_t n) {
  Table t = {allocate((size_t)numBlocks(n) * sizeof(Block))};
  memset(t.blocks, 0, (size_t)numBlocks(n) * sizeof(Block));
  for (int64_t i = 0; i < n; ++i) {
    Block *blk = &t.blocks[i / W];
    float *fields[FIELDS] = {blk->x, blk->y, blk->dx, blk->dy, blk->last};
    for (int f = 0; f < FIELDS; ++f)
      fields[f][i % W] = initial(a, f, i);
  }
  return t;
}
static float get(Table *t, int f, int64_t i) {
  Block *blk = &t->blocks[i / W];
  float *fields[FIELDS] = {blk->x, blk->y, blk->dx, blk->dy, blk->last};
  return fields[f][i % W];
}

static void frame(Table *b, Table *p, int64_t n, float dt) {
  int64_t blocks = numBlocks(n);
  Block *bb = b->blocks;
#pragma omp parallel for schedule(static)
  for (int64_t k = 0; k < blocks; ++k)
    for (int j = 0; j < W; ++j) {
      bb[k].dy[j] = bb[k].dy[j] - G * dt;
      bb[k].x[j] = bb[k].x[j] + bb[k].dx[j] * dt;
      bb[k].y[j] = bb[k].y[j] + bb[k].dy[j] * dt;
    }
  Block *pb = p->blocks;
#pragma omp parallel for schedule(static)
  for (int64_t k = 0; k < blocks; ++k)
    for (int j = 0; j < W; ++j) {
      pb[k].dx[j] = pb[k].dx[j] + WIND * dt;
      pb[k].x[j] = pb[k].x[j] + pb[k].dx[j] * dt;
      pb[k].y[j] = pb[k].y[j] + pb[k].dy[j] * dt;
      pb[k].last[j] = pb[k].last[j] - dt;
    }
}
#endif

static uint64_t now(void) {
#ifdef __APPLE__
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <entities> <repetitions> <target ms>\n",
            argv[0]);
    return 2;
  }
  int64_t n = atoll(argv[1]);
  int reps = atoi(argv[2]);
  uint64_t target = (uint64_t)atoll(argv[3]) * 1000000;
  const float dt = 1.0f / 60.0f;

  Table bodies = makeTable(0, n), particles = makeTable(1, n);
  for (int f = 0; f < VALIDATION_FRAMES; ++f)
    frame(&bodies, &particles, n, dt);
  double checksum = 0;
  for (int f = 0; f < FIELDS; ++f)
    for (int64_t i = 0; i < n; ++i)
      checksum += (double)get(&bodies, f, i) + (double)get(&particles, f, i);

  int64_t frames = 1;
  for (;;) {
    uint64_t start = now();
    for (int64_t f = 0; f < frames; ++f)
      frame(&bodies, &particles, n, dt);
    uint64_t elapsed = now() - start;
    if (elapsed * 10 >= target) {
      frames = (int64_t)((double)frames * target / elapsed) + 1;
      break;
    }
    frames *= 2;
  }
  uint64_t best = UINT64_MAX;
  for (int r = 0; r < reps; ++r) {
    uint64_t start = now();
    for (int64_t f = 0; f < frames; ++f)
      frame(&bodies, &particles, n, dt);
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }
  printf("n=%lld frames=%lld ns_per_frame=%.1f checksum=%.17g\n",
         (long long)n, (long long)frames, (double)best / frames, checksum);
  return 0;
}
