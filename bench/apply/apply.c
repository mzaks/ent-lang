// Cross-entity writes: what ent.apply costs against hand-written C.
//
// N guns each deal damage (1 to 4, exact in f32 in any order) to one of M
// ships, chosen at random with a fixed seed; a frame applies every gun
// once: hp[target] -= damage. Every variant runs the same frames on the
// same targets, so all must end with the same checksum.
//
//   -DVARIANT=0  c-index: hp[t[i]] -= d[i], sequential, targets are rows
//   -DVARIANT=1  c-atomic-par: the same in an OpenMP loop, with an atomic
//                float add (compare-and-swap); parallel, but the order of
//                the adds is not fixed
//   -DVARIANT=2  c-buffered: what the compiled program does, without ids:
//                an OpenMP loop fills (target row, value) per gun, then one
//                sequential loop combines them
//   -DVARIANT=3  compiled: bench/apply/fire*.mlir lowered by ent-opt and
//                linked in; targets are entity ids (Rows or generational,
//                depending on the program)
//
// Usage: apply <guns> <ships> <frames> <repetitions>
// Prints the best repetition's time per frame and a checksum.

#if VARIANT == 3
#include "fire_world.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef VARIANT
#define VARIANT 0
#endif

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint32_t rng(uint32_t bound) {
  rngState ^= rngState >> 12;
  rngState ^= rngState << 25;
  rngState ^= rngState >> 27;
  return (uint32_t)(((rngState * 0x2545F4914F6CDD1Dull) >> 32) % bound);
}

static void *allocate(size_t bytes) {
  void *p;
  if (posix_memalign(&p, 64, bytes ? bytes : 64) != 0) {
    perror("posix_memalign");
    exit(1);
  }
  return p;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s guns ships frames repetitions\n", argv[0]);
    return 2;
  }
  int64_t n = atoll(argv[1]), m = atoll(argv[2]);
  int frames = atoi(argv[3]), reps = atoi(argv[4]);
  uint32_t *targetRow = allocate(sizeof(uint32_t) * n);
  float *damage = allocate(sizeof(float) * n);
  for (int64_t i = 0; i < n; ++i) {
    targetRow[i] = rng((uint32_t)m);
    damage[i] = (float)(1 + i % 4);
  }

#if VARIANT == 3
  ent_world *w = ent_world_create();
  if (m > ENT_Ship_CAPACITY || n > ENT_Turret_CAPACITY) {
    fprintf(stderr, "sizes exceed the program's capacities\n");
    return 2;
  }
  ent_entity *shipIds = allocate(sizeof(ent_entity) * m);
  for (int64_t s = 0; s < m; ++s)
    shipIds[s] = ent_Ship_spawn(w);
  for (int64_t i = 0; i < n; ++i) {
    int64_t row = ent_entity_row(w, ent_Turret_spawn(w));
    ent_Turret_Gun_damage(w)[row] = damage[i];
    ent_Turret_Gun_target(w)[row] = shipIds[targetRow[i]];
  }
  float *hp = ent_Ship_Hull_hp(w);
#else
  float *hp = allocate(sizeof(float) * m);
#endif
#if VARIANT == 2
  uint32_t *bufferRow = allocate(sizeof(uint32_t) * n);
  float *bufferValue = allocate(sizeof(float) * n);
#endif

  uint64_t best = UINT64_MAX;
  for (int rep = 0; rep < reps; ++rep) {
    for (int64_t s = 0; s < m; ++s)
      hp[s] = 0;
    uint64_t start = now();
    for (int frame = 0; frame < frames; ++frame) {
#if VARIANT == 0
      for (int64_t i = 0; i < n; ++i)
        hp[targetRow[i]] -= damage[i];
#elif VARIANT == 1
#pragma omp parallel for
      for (int64_t i = 0; i < n; ++i) {
        uint32_t *cell = (uint32_t *)&hp[targetRow[i]];
        uint32_t old = __atomic_load_n(cell, __ATOMIC_RELAXED), next;
        do {
          union { uint32_t bits; float value; } u = {old};
          u.value -= damage[i];
          next = u.bits;
        } while (!__atomic_compare_exchange_n(cell, &old, next, 1,
                                              __ATOMIC_RELAXED,
                                              __ATOMIC_RELAXED));
      }
#elif VARIANT == 2
#pragma omp parallel for
      for (int64_t i = 0; i < n; ++i) {
        bufferRow[i] = targetRow[i];
        bufferValue[i] = -damage[i];
      }
      for (int64_t i = 0; i < n; ++i)
        hp[bufferRow[i]] += bufferValue[i];
#else
      ent_frame(w);
#endif
    }
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }

  double checksum = 0;
  for (int64_t s = 0; s < m; ++s)
    checksum += (double)hp[s] * (double)(1 + s % 13);
  printf("ns_per_frame=%.1f checksum=%.17g\n", (double)best / frames,
         checksum);
  return 0;
}
