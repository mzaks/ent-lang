// Four mixers for a hash index's table (bench/RESULTS.md, 2026-10-23):
// linear probing, slots a power of two at least twice the keys, a key's
// home the hash's low bits, as `index ... hash` has its slots. 200,000
// keys made into a table and each found, and as many not there looked
// for; the best of 15. (The last set, 7 keys for 200,000 entities, is a
// slot an entity, as the index had it first: one run of slots.)
//   clang -O2 -march=native mixers.c -o mixers && taskset -c 2 ./mixers
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned __int128 u128;
static inline uint64_t mum(uint64_t a, uint64_t b) {
  u128 r = (u128)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

// rapidhash's mix, as ent.hash lowers it.
static inline uint64_t h_rapid(uint64_t x) {
  return mum(x ^ 0x2d358dccaa6c78a5ull,
             0xbdd89aa982704029ull ^ 0x8bb84b93962eacc9ull);
}
// wyhash64 (wyhash.h's wyhash64(A, B) with B the seed).
static inline uint64_t h_wy(uint64_t x) {
  uint64_t A = x ^ 0x2d358dccaa6c78a5ull,
           B = 0xbdd89aa982704029ull ^ 0x8bb84b93962eacc9ull;
  u128 r = (u128)A * B;
  A = (uint64_t)r;
  B = (uint64_t)(r >> 64);
  return mum(A ^ 0x2d358dccaa6c78a5ull, B ^ 0x8bb84b93962eacc9ull);
}
// aHash's fallback (no AES): folded multiply in, folded multiply and a
// rotate out.
static inline uint64_t h_ahash(uint64_t x) {
  const uint64_t MULTIPLE = 6364136223846793005ull;
  uint64_t buffer = 0x243f6a8885a308d3ull, pad = 0x13198a2e03707344ull;
  buffer = mum(x ^ buffer, MULTIPLE);
  uint64_t rot = buffer & 63, f = mum(buffer, pad);
  return (f << rot) | (f >> ((64 - rot) & 63));
}
// SplitMix64's finalizer.
static inline uint64_t h_split(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

#define N 200000
#define SLOTS (1 << 19)
static int64_t keys[SLOTS];
static int32_t who[SLOTS];
static int64_t input[N], misses[N];

#define RUN(NAME, H)                                                           \
  static void run_##NAME(const char *set, double *build, double *find,         \
                         double *probes, int *longest) {                       \
    double b = 1e9, f = 1e9;                                                   \
    uint64_t steps = 0;                                                        \
    int most = 0;                                                              \
    volatile int64_t sink = 0;                                                 \
    for (int rep = 0; rep < 15; ++rep) {                                       \
      double t0 = now();                                                       \
      memset(who, 0xff, sizeof who);                                           \
      for (int i = 0; i < N; ++i) {                                            \
        uint64_t s = H((uint64_t)input[i]) & (SLOTS - 1);                      \
        while (who[s] >= 0)                                                    \
          s = (s + 1) & (SLOTS - 1);                                           \
        keys[s] = input[i];                                                    \
        who[s] = i;                                                            \
      }                                                                        \
      double t1 = now();                                                       \
      steps = 0;                                                               \
      most = 0;                                                                \
      int64_t acc = 0;                                                         \
      for (int i = 0; i < N; ++i) {                                            \
        for (int pass = 0; pass < 2; ++pass) {                                 \
          int64_t k = pass ? misses[i] : input[i];                             \
          uint64_t s = H((uint64_t)k) & (SLOTS - 1);                           \
          int len = 0;                                                         \
          while (who[s] >= 0) {                                                \
            if (keys[s] == k)                                                  \
              acc += who[s];                                                   \
            s = (s + 1) & (SLOTS - 1);                                         \
            ++len;                                                             \
          }                                                                    \
          steps += len;                                                        \
          if (len > most)                                                      \
            most = len;                                                        \
        }                                                                      \
      }                                                                        \
      double t2 = now();                                                       \
      sink += acc;                                                             \
      if (t1 - t0 < b)                                                         \
        b = t1 - t0;                                                           \
      if (t2 - t1 < f)                                                         \
        f = t2 - t1;                                                           \
    }                                                                          \
    *build = b;                                                                \
    *find = f;                                                                 \
    *probes = (double)steps / (2.0 * N);                                       \
    *longest = most;                                                           \
  }
RUN(rapid, h_rapid)
RUN(wy, h_wy) RUN(ahash, h_ahash) RUN(split, h_split)

    static uint64_t state = 1;
static uint64_t next(void) {
  state += 0x9e3779b97f4a7c15ull;
  return h_split(state);
}

int main(void) {
  const char *sets[] = {"sequential", "random", "stride 1024", "high bits",
                        "teams of 7"};
  for (int set = 0; set < 5; ++set) {
    for (int i = 0; i < N; ++i) {
      switch (set) {
      case 0:
        input[i] = i;
        misses[i] = N + i;
        break;
      case 1:
        input[i] = (int64_t)next();
        misses[i] = (int64_t)next();
        break;
      case 2:
        input[i] = (int64_t)i * 1024;
        misses[i] = (int64_t)i * 1024 + 1;
        break;
      case 3:
        input[i] = (int64_t)i << 40;
        misses[i] = ((int64_t)(N + i)) << 40;
        break;
      case 4:
        input[i] = i % 7;
        misses[i] = 7 + i % 7;
        break;
      }
    }
    struct {
      const char *name;
      void (*run)(const char *, double *, double *, double *, int *);
    } mixers[] = {{"rapid", run_rapid},
                  {"wyhash64", run_wy},
                  {"ahash-fallback", run_ahash},
                  {"splitmix64", run_split}};
    for (int m = 0; m < 4; ++m) {
      double b, f, p;
      int most;
      if (set == 4 && m > 0) {
      } // (all mixers alike: one run each still)
      mixers[m].run(sets[set], &b, &f, &p, &most);
      printf("%-12s %-15s build %6.2f ms  find+miss %7.2f ms  probes %8.2f  "
             "longest %d\n",
             sets[set], mixers[m].name, b * 1e3, f * 1e3, p, most);
    }
  }
}
