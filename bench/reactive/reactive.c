// Change detection: what reacting to changed components costs under
// different tracking schemes.
//
// N units have hp, a seed and a bar (width, visits). Each frame:
//   1. hit:    the units a hash of their seed and the frame picks (threshold
//              / 65536 of them) lose 1 hp;
//   2. redraw: the units whose hp changed get width = f(hp) and visits += 1.
// f depends on hp only, so redrawing an unchanged unit only adds a visit:
// every variant ends with the same widths (the checksum), while visits show
// how many units a variant redrew that it need not have.
//
//   -DVARIANT=0  poll: no tracking; redraw every unit every frame
//   -DVARIANT=1  rowstamp: hit stores a tick per changed unit; redraw scans
//                the ticks and redraws the newer ones (the scheme the
//                compiler uses)
//   -DVARIANT=2  blockstamp: as rowstamp, plus the newest tick per block of
//                64 units; redraw skips blocks with nothing newer
//   -DVARIANT=3  collector: hit appends a changed unit to a list, once (a
//                flag per unit), as an Entitas collector does; redraw walks
//                the list
//   -DVARIANT=4  bevy: as rowstamp, but every component carries ticks and
//                every mutable write stamps them, observed or not (redraw
//                stamps the bar)
//   -DVARIANT=5  unity: a version per chunk of 128 units and component,
//                bumped for every chunk a system has write access to,
//                written or not; redraw handles the chunks whose hp version
//                is newer (here: all, since hit has write access to all)
//   -DVARIANT=6  compiled: bench/reactive/react.mlir, lowered by ent-opt;
//                run.py builds it with the trigger's event log (walk the
//                changed units, scan when it overflows) and without
//   -DHEAVY      f is a 32-step loop instead of one multiply-add
//
// Usage: reactive <units> <threshold> <frames> <repetitions>
// Prints the best repetition's time per frame, the redraws per frame in
// the last repetition, and a checksum.

#if VARIANT == 6
#include "react_world.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma clang fp contract(off)

#ifndef VARIANT
#define VARIANT 0
#endif

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void *allocate(size_t bytes) {
  void *p;
  if (posix_memalign(&p, 64, bytes ? bytes : 64) != 0) {
    perror("posix_memalign");
    exit(1);
  }
  memset(p, 0, bytes);
  return p;
}

static inline int chosen(int32_t seed, int32_t frame, int32_t threshold) {
  uint32_t x = (uint32_t)seed * 2654435761u + (uint32_t)frame * 40503u;
  return ((x >> 8) & 65535u) < (uint32_t)threshold;
}

static inline float bar(float hp) {
#ifdef HEAVY
  float w = hp;
  for (int k = 0; k < 32; ++k)
    w = w * 0.999f + 0.001f;
  return w;
#else
  return hp * 0.5f + 1.0f;
#endif
}

enum { BLOCK = 64, CHUNK = 128 };

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s units threshold frames repetitions\n",
            argv[0]);
    return 2;
  }
  int64_t n = atoll(argv[1]);
  int32_t threshold = atoi(argv[2]);
  int frames = atoi(argv[3]), reps = atoi(argv[4]);

#if VARIANT == 6
  ent_world *world = ent_world_create();
  if (n > ENT_Unit_CAPACITY) {
    fprintf(stderr, "more units than the program's capacity\n");
    return 2;
  }
  ent_Unit_spawn_n(world, n);
  float *hp = ent_Unit_Hull_hp(world);
  int32_t *seed = ent_Unit_Seed_s(world);
  float *width = ent_Unit_Bar_width(world);
  int32_t *visits = ent_Unit_Bar_visits(world);
#else
  float *hp = allocate(sizeof(float) * n);
  int32_t *seed = allocate(sizeof(int32_t) * n);
  float *width = allocate(sizeof(float) * n);
  int32_t *visits = allocate(sizeof(int32_t) * n);
  // Ticks as the compiler keeps them: events are stamped with counter + 1;
  // a redraw sees what is newer than the tick it last started at, then
  // makes counter + 1 its new last tick and the counter. New units count as
  // changed (stamp 1), so the first redraw sees them all.
  int64_t counter = 0, last = 0;
  int64_t *stamp = allocate(sizeof(int64_t) * n);
  for (int64_t i = 0; i < n; ++i)
    stamp[i] = 1;
#if VARIANT == 2
  int64_t blocks = (n + BLOCK - 1) / BLOCK;
  int64_t *blockStamp = allocate(sizeof(int64_t) * blocks);
  for (int64_t b = 0; b < blocks; ++b)
    blockStamp[b] = 1;
#elif VARIANT == 3
  uint8_t *collected = allocate(n);
  int32_t *list = allocate(sizeof(int32_t) * n);
  int64_t listed = 0;
  for (int64_t i = 0; i < n; ++i) {
    collected[i] = 1;
    list[listed++] = (int32_t)i;
  }
#elif VARIANT == 4
  int64_t *barStamp = allocate(sizeof(int64_t) * n);
#elif VARIANT == 5
  int64_t chunks = (n + CHUNK - 1) / CHUNK;
  int64_t *hpVersion = allocate(sizeof(int64_t) * chunks);
  int64_t *barVersion = allocate(sizeof(int64_t) * chunks);
  for (int64_t c = 0; c < chunks; ++c)
    hpVersion[c] = 1;
#endif
#endif
  for (int64_t i = 0; i < n; ++i) {
    hp[i] = 1000;
    seed[i] = (int32_t)i;
    width[i] = 0;
    visits[i] = 0;
  }

  uint64_t best = UINT64_MAX;
  int64_t visitsBefore = 0;
  int32_t frame = 0;
  for (int rep = 0; rep < reps; ++rep) {
    if (rep == reps - 1)
      for (int64_t i = 0; i < n; ++i)
        visitsBefore += visits[i];
    uint64_t start = now();
    for (int f = 0; f < frames; ++f, ++frame) {
#if VARIANT == 6
      ent_frame(world, frame, threshold);
#else
      // hit
      int64_t tick = counter + 1;
      (void)tick; // poll and collector keep no ticks
      for (int64_t i = 0; i < n; ++i) {
        if (!chosen(seed[i], frame, threshold))
          continue;
        hp[i] -= 1.0f;
#if VARIANT == 1 || VARIANT == 4
        stamp[i] = tick;
#elif VARIANT == 2
        stamp[i] = tick;
        blockStamp[i / BLOCK] = tick;
#elif VARIANT == 3
        if (!collected[i]) {
          collected[i] = 1;
          list[listed++] = (int32_t)i;
        }
#endif
      }
#if VARIANT == 5
      // hit had write access to every chunk's hp.
      for (int64_t c = 0; c < chunks; ++c)
        hpVersion[c] = tick;
#endif
      // redraw
      int64_t seen = last;
      last = counter = counter + 1;
#if VARIANT == 0
      (void)seen;
      for (int64_t i = 0; i < n; ++i) {
        width[i] = bar(hp[i]);
        visits[i] += 1;
      }
#elif VARIANT == 1
      for (int64_t i = 0; i < n; ++i)
        if (stamp[i] > seen) {
          width[i] = bar(hp[i]);
          visits[i] += 1;
        }
#elif VARIANT == 2
      for (int64_t b = 0; b < blocks; ++b) {
        if (blockStamp[b] <= seen)
          continue;
        int64_t end = (b + 1) * BLOCK < n ? (b + 1) * BLOCK : n;
        for (int64_t i = b * BLOCK; i < end; ++i)
          if (stamp[i] > seen) {
            width[i] = bar(hp[i]);
            visits[i] += 1;
          }
      }
#elif VARIANT == 3
      (void)seen;
      for (int64_t k = 0; k < listed; ++k) {
        int32_t i = list[k];
        collected[i] = 0;
        width[i] = bar(hp[i]);
        visits[i] += 1;
      }
      listed = 0;
#elif VARIANT == 4
      int64_t redrawTick = counter + 1;
      for (int64_t i = 0; i < n; ++i)
        if (stamp[i] > seen) {
          width[i] = bar(hp[i]);
          visits[i] += 1;
          barStamp[i] = redrawTick;
        }
#elif VARIANT == 5
      int64_t redrawTick = counter + 1;
      for (int64_t c = 0; c < chunks; ++c) {
        if (hpVersion[c] <= seen)
          continue;
        int64_t end = (c + 1) * CHUNK < n ? (c + 1) * CHUNK : n;
        for (int64_t i = c * CHUNK; i < end; ++i) {
          width[i] = bar(hp[i]);
          visits[i] += 1;
        }
        barVersion[c] = redrawTick;
      }
#endif
#endif
    }
    uint64_t elapsed = now() - start;
    if (elapsed < best)
      best = elapsed;
  }

  int64_t visitsAfter = 0;
  double checksum = 0;
  for (int64_t i = 0; i < n; ++i) {
    visitsAfter += visits[i];
    checksum += (double)width[i] * (double)(1 + i % 13) + (double)hp[i];
  }
  printf("ns_per_frame=%.1f redraws_per_frame=%.1f checksum=%.17g\n",
         (double)best / frames,
         (double)(visitsAfter - visitsBefore) / frames, checksum);
  return 0;
}
