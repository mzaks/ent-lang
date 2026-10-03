// Single-loop read-modify-write over K arrays, a[k][i] += s, with the same
// total number of bytes for every K. Measures how bandwidth depends on the
// number of concurrent streams, the one thing an AoSoA layout changes for
// the example's loops (5 streams per archetype in SoA, 1 in AoSoA).
//
//   -DK=<streams>, optionally -fopenmp, and -DSTAGGER to offset each
//   array by 17 more cache lines than the previous one
// Usage: streams <total floats> <repetitions> <target ms>
// Prints: n=<floats per stream> ns_per_pass=<best> gb_per_s=<bytes moved>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef K
#define K 1
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

static void pass(float *a[K], int64_t n, float s) {
#pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < n; ++i)
    for (int k = 0; k < K; ++k)
      a[k][i] += s;
}

int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  int64_t n = atoll(argv[1]) / K;
  int reps = atoi(argv[2]);
  uint64_t target = (uint64_t)atoll(argv[3]) * 1000000;
  float *a[K];
  for (int k = 0; k < K; ++k) {
    if (posix_memalign((void **)&a[k], 64,
                       (size_t)n * sizeof(float) + 64 * 17 * K))
      return 1;
#ifdef STAGGER
    a[k] += 16 * 17 * k;
#endif
    for (int64_t i = 0; i < n; ++i)
      a[k][i] = (float)(i % 7);
  }
  int64_t passes = 1;
  for (;;) {
    uint64_t start = now();
    for (int64_t p = 0; p < passes; ++p)
      pass(a, n, 1e-3f);
    uint64_t elapsed = now() - start;
    if (elapsed * 10 >= target) {
      passes = (int64_t)((double)passes * target / elapsed) + 1;
      break;
    }
    passes *= 2;
  }
  uint64_t best = UINT64_MAX;
  for (int r = 0; r < reps; ++r) {
    uint64_t start = now();
    for (int64_t p = 0; p < passes; ++p)
      pass(a, n, 1e-3f);
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }
  double ns = (double)best / passes;
  // Each element is read once and written once.
  printf("n=%lld ns_per_pass=%.1f gb_per_s=%.1f\n", (long long)n, ns,
         2.0 * K * n * sizeof(float) / ns);
  return 0;
}
