// Benchmark host for examples/integrate.mlir: runs the lowered `frame`
// schedule (or a hand-written reference with the same signature) over N
// entities per archetype and reports the best time per frame.
//
// Usage: bench <entities per archetype> <repetitions> <target ms>
// Prints: n=<N> frames=<F> ns_per_frame=<best> checksum=<sum of all columns>
//
// The checksum comes from a fixed validation run of VALIDATION_FRAMES
// frames on fresh columns, so it is comparable across variants: fusion and
// parallel loops keep the order of floating point operations per entity.
// Timing then continues on the same columns with a frame count calibrated
// so that one repetition takes about <target ms>.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// The initial values must not depend on build flags: without this, clang
// contracts `base + step * i` into a fused multiply-add in some builds and
// not in others, and the checksums of equivalent variants differ.
#pragma clang fp contract(off)

typedef struct {
  float *allocated;
  float *aligned;
  int64_t offset;
  int64_t size;
  int64_t stride;
} Column;

void _mlir_ciface_frame(float dt, int64_t bodies, Column *bx, Column *by,
                        Column *bdx, Column *bdy, Column *bkg,
                        int64_t particles, Column *px, Column *py,
                        Column *pdx, Column *pdy, Column *plife,
                        int64_t scenery, Column *sx, Column *sy);

enum { NUM_COLUMNS = 12 };

static Column makeColumn(int64_t n, float base, float step) {
  float *data;
  if (posix_memalign((void **)&data, 64, (size_t)n * sizeof(float)) != 0) {
    perror("posix_memalign");
    exit(1);
  }
  for (int64_t i = 0; i < n; ++i)
    data[i] = base + step * (float)(i % 1024);
  return (Column){data, data, 0, n, 1};
}

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

enum { VALIDATION_FRAMES = 10 };

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <entities> <repetitions> <target ms>\n",
            argv[0]);
    return 2;
  }
  int64_t n = atoll(argv[1]);
  int reps = atoi(argv[2]);
  uint64_t target = (uint64_t)atoll(argv[3]) * 1000000;

  // Body: Position, Velocity, Mass; Particle: Position, Velocity, Lifetime;
  // Scenery: Position.
  Column c[NUM_COLUMNS] = {
      makeColumn(n, 0, 0.5f),   makeColumn(n, 100, 0.25f),
      makeColumn(n, 1, 0.01f),  makeColumn(n, 0, 0.02f),
      makeColumn(n, 1, 0.1f),   makeColumn(n, 0, 0.5f),
      makeColumn(n, 0, 0.25f),  makeColumn(n, 2, -0.01f),
      makeColumn(n, 1, 0.02f),  makeColumn(n, 10, 0.1f),
      makeColumn(n, 7, 0.5f),   makeColumn(n, 7, 0.25f),
  };
  const float dt = 1.0f / 60.0f;
#define FRAME()                                                                \
  _mlir_ciface_frame(dt, n, &c[0], &c[1], &c[2], &c[3], &c[4], n, &c[5],       \
                     &c[6], &c[7], &c[8], &c[9], n, &c[10], &c[11])

  for (int f = 0; f < VALIDATION_FRAMES; ++f)
    FRAME();
  double checksum = 0;
  for (int k = 0; k < NUM_COLUMNS; ++k)
    for (int64_t i = 0; i < n; ++i)
      checksum += c[k].aligned[i];

  // Calibrate (this also warms caches and the OpenMP thread pool): double
  // the frame count until a batch takes at least a tenth of the target.
  int64_t frames = 1;
  for (;;) {
    uint64_t start = now();
    for (int64_t f = 0; f < frames; ++f)
      FRAME();
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
      FRAME();
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }

  printf("n=%lld frames=%lld ns_per_frame=%.1f checksum=%.17g\n",
         (long long)n, (long long)frames, (double)best / frames, checksum);
  return 0;
}
