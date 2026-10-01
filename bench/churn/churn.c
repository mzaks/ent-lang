// Structural churn: what it costs to add and remove an optional component
// under different storage schemes.
//
// N entities have Position (x, y) and Velocity (dx, dy). A fraction of them
// also have Status (remaining: f32), a timed effect. Each frame:
//   1. churn:  churn * N / 2 holders lose Status, as many others gain it
//              (remaining = 5), so the density stays constant;
//   2. move:   every entity:        x += dx * dt; y += dy * dt
//   3. status: every Status holder: dx *= 0.99; remaining -= dt
// Which entities change is precomputed and the same for every variant, so
// all variants must end with the same checksum.
//
//   -DVARIANT=0  archetypes: two tables; gaining or losing Status moves the
//                entity to the other table (copy, then swap-remove)
//   -DVARIANT=1  wide: one table with a Status column and a presence byte;
//                status runs over everyone and selects (branch-free)
//   -DVARIANT=2  wide as 1, but status branches on the presence byte
//   -DVARIANT=3  sparse set: Status packed densely with its owner's id and
//                a sparse id -> slot index; status reads and writes the
//                owners' Velocity through the id
//   -DSTAGGER    offset every allocation by 17 more cache lines than the
//                previous one, as the compiled world lays out its columns
//   -DWIDTH16    with VARIANT=1, vectorise the status loop 16 wide, as LLVM
//                chooses for the compiled program (it picks 4 x 2 here)
//   -DVARIANT=4  compiled: bench/churn/status.mlir, where Status is an
//                optional component, lowered by ent-opt and linked in; the
//                host changes presence through the generated header
//
// Usage: churn <entities> <density> <churn per frame> <frames> <repetitions>
// Prints the best repetition: total, churn and systems time per frame, the
// bytes the variant allocates, and a checksum.

#if VARIANT == 4
#include "status_world.h"
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

static const float DT = 1.0f / 60.0f, SLOW = 0.99f, DURATION = 5.0f;

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint32_t rng(uint32_t bound) {
  rngState ^= rngState >> 12;
  rngState ^= rngState << 25;
  rngState ^= rngState >> 27;
  return (uint32_t)(((rngState * 0x2545F4914F6CDD1Dull) >> 32) % bound);
}

static size_t allocated = 0;
static void *allocate(size_t bytes) {
#ifdef STAGGER
  static size_t count = 0;
  size_t shift = 17 * 64 * count++;
#else
  size_t shift = 0;
#endif
  char *p;
  if (posix_memalign((void **)&p, 64, (bytes ? bytes : 64) + shift) != 0) {
    perror("posix_memalign");
    exit(1);
  }
  allocated += bytes;
  return p + shift;
}

static float initialX(uint32_t id) { return 0.5f * (float)(id % 1024); }
static float initialY(uint32_t id) { return 0.25f * (float)(id % 512); }
static float initialDx(uint32_t id) { return 1.0f + 0.1f * (float)(id % 7); }
static float initialDy(uint32_t id) { return 0.5f; }

//===----------------------------------------------------------------------===//
// Storage variants. Each provides: init, gain(id), lose(id), systems(), and
// read access by id for the checksum.
//===----------------------------------------------------------------------===//

static int64_t N;

#if VARIANT == 0
typedef struct {
  float *x, *y, *dx, *dy, *remaining; // remaining only in the Status table
  uint32_t *id;
  int64_t count;
} Table;
static Table plain, status;
static uint8_t *tableOf; // 0 plain, 1 status
static uint32_t *rowOf;

static void makeTable(Table *t, int withStatus) {
  t->x = allocate(N * 4), t->y = allocate(N * 4);
  t->dx = allocate(N * 4), t->dy = allocate(N * 4);
  t->remaining = withStatus ? allocate(N * 4) : NULL;
  t->id = allocate(N * 4);
  t->count = 0;
}
static void storageInit(const uint8_t *holds) {
  makeTable(&plain, 0), makeTable(&status, 1);
  tableOf = allocate(N), rowOf = allocate(N * 4);
  for (uint32_t id = 0; id < N; ++id) {
    Table *t = holds[id] ? &status : &plain;
    int64_t r = t->count++;
    t->x[r] = initialX(id), t->y[r] = initialY(id);
    t->dx[r] = initialDx(id), t->dy[r] = initialDy(id);
    if (holds[id])
      t->remaining[r] = DURATION;
    t->id[r] = id;
    tableOf[id] = holds[id], rowOf[id] = (uint32_t)r;
  }
}
// Append entity `id` (at row `r` of `from`) to `to`, then fill the hole in
// `from` with its last row.
static void move(uint32_t id, Table *from, Table *to, uint8_t toIndex) {
  int64_t r = rowOf[id], w = to->count++;
  to->x[w] = from->x[r], to->y[w] = from->y[r];
  to->dx[w] = from->dx[r], to->dy[w] = from->dy[r];
  if (to->remaining)
    to->remaining[w] = DURATION;
  to->id[w] = id;
  tableOf[id] = toIndex, rowOf[id] = (uint32_t)w;

  int64_t last = --from->count;
  if (r != last) {
    from->x[r] = from->x[last], from->y[r] = from->y[last];
    from->dx[r] = from->dx[last], from->dy[r] = from->dy[last];
    if (from->remaining)
      from->remaining[r] = from->remaining[last];
    uint32_t moved = from->id[last];
    from->id[r] = moved, rowOf[moved] = (uint32_t)r;
  }
}
static void gain(uint32_t id) { move(id, &plain, &status, 1); }
static void lose(uint32_t id) { move(id, &status, &plain, 0); }
static void moveAll(Table *t) {
  float *x = t->x, *y = t->y, *dx = t->dx, *dy = t->dy;
  for (int64_t i = 0; i < t->count; ++i) {
    x[i] = x[i] + dx[i] * DT;
    y[i] = y[i] + dy[i] * DT;
  }
}
static void systems(void) {
  moveAll(&plain), moveAll(&status);
  float *dx = status.dx, *remaining = status.remaining;
  for (int64_t i = 0; i < status.count; ++i) {
    dx[i] = dx[i] * SLOW;
    remaining[i] = remaining[i] - DT;
  }
}
static void readEntity(uint32_t id, float out[5]) {
  Table *t = tableOf[id] ? &status : &plain;
  uint32_t r = rowOf[id];
  out[0] = t->x[r], out[1] = t->y[r], out[2] = t->dx[r], out[3] = t->dy[r];
  out[4] = tableOf[id] ? t->remaining[r] : 0;
}

#elif VARIANT == 1 || VARIANT == 2
static float *x, *y, *dx, *dy, *remaining;
static uint8_t *present;
static void storageInit(const uint8_t *holds) {
  x = allocate(N * 4), y = allocate(N * 4), dx = allocate(N * 4);
  dy = allocate(N * 4), remaining = allocate(N * 4);
  present = allocate(N);
  for (uint32_t id = 0; id < N; ++id) {
    x[id] = initialX(id), y[id] = initialY(id);
    dx[id] = initialDx(id), dy[id] = initialDy(id);
    remaining[id] = DURATION;
    present[id] = holds[id];
  }
}
static void gain(uint32_t id) { present[id] = 1, remaining[id] = DURATION; }
static void lose(uint32_t id) { present[id] = 0; }
static void systems(void) {
  for (int64_t i = 0; i < N; ++i) {
    x[i] = x[i] + dx[i] * DT;
    y[i] = y[i] + dy[i] * DT;
  }
#if VARIANT == 1
#ifdef WIDTH16
#pragma clang loop vectorize_width(16)
#endif
  for (int64_t i = 0; i < N; ++i) {
    int p = present[i];
    float slowed = dx[i] * SLOW, left = remaining[i] - DT;
    dx[i] = p ? slowed : dx[i];
    remaining[i] = p ? left : remaining[i];
  }
#else
  for (int64_t i = 0; i < N; ++i)
    if (present[i]) {
      dx[i] = dx[i] * SLOW;
      remaining[i] = remaining[i] - DT;
    }
#endif
}
static void readEntity(uint32_t id, float out[5]) {
  out[0] = x[id], out[1] = y[id], out[2] = dx[id], out[3] = dy[id];
  out[4] = present[id] ? remaining[id] : 0;
}

#elif VARIANT == 3
static float *x, *y, *dx, *dy;
static float *remaining; // dense, by slot
static uint32_t *owner;  // dense, by slot
static int32_t *slotOf;  // sparse, by id; -1 if absent
static int64_t holders;
static void storageInit(const uint8_t *holds) {
  x = allocate(N * 4), y = allocate(N * 4), dx = allocate(N * 4);
  dy = allocate(N * 4);
  remaining = allocate(N * 4), owner = allocate(N * 4);
  slotOf = allocate(N * 4);
  holders = 0;
  for (uint32_t id = 0; id < N; ++id) {
    x[id] = initialX(id), y[id] = initialY(id);
    dx[id] = initialDx(id), dy[id] = initialDy(id);
    slotOf[id] = -1;
    if (holds[id]) {
      slotOf[id] = (int32_t)holders;
      owner[holders] = id, remaining[holders] = DURATION;
      ++holders;
    }
  }
}
static void gain(uint32_t id) {
  slotOf[id] = (int32_t)holders;
  owner[holders] = id, remaining[holders] = DURATION;
  ++holders;
}
static void lose(uint32_t id) {
  int32_t s = slotOf[id];
  int64_t last = --holders;
  if (s != last) {
    remaining[s] = remaining[last];
    owner[s] = owner[last];
    slotOf[owner[s]] = s;
  }
  slotOf[id] = -1;
}
static void systems(void) {
  for (int64_t i = 0; i < N; ++i) {
    x[i] = x[i] + dx[i] * DT;
    y[i] = y[i] + dy[i] * DT;
  }
  for (int64_t s = 0; s < holders; ++s) {
    uint32_t id = owner[s];
    dx[id] = dx[id] * SLOW;
    remaining[s] = remaining[s] - DT;
  }
}
static void readEntity(uint32_t id, float out[5]) {
  out[0] = x[id], out[1] = y[id], out[2] = dx[id], out[3] = dy[id];
  out[4] = slotOf[id] >= 0 ? remaining[slotOf[id]] : 0;
}

#elif VARIANT == 4
static ent_world *world;
static float *remaining;
static uint8_t *present;
static void storageInit(const uint8_t *holds) {
  // A fresh world per repetition, like the other variants' fresh storage.
  world = ent_world_create();
  if (!world || !ent_Character_spawn_n(world, N)) {
    fprintf(stderr, "n=%lld does not fit the world\n", (long long)N);
    exit(1);
  }
  allocated += ENT_WORLD_BYTES;
  float *x = ent_Character_Position_x(world);
  float *y = ent_Character_Position_y(world);
  float *dx = ent_Character_Velocity_dx(world);
  float *dy = ent_Character_Velocity_dy(world);
  remaining = ent_Character_Status_remaining(world);
  present = ent_Character_Status_present(world);
  for (uint32_t id = 0; id < N; ++id) {
    x[id] = initialX(id), y[id] = initialY(id);
    dx[id] = initialDx(id), dy[id] = initialDy(id);
    remaining[id] = DURATION;
    present[id] = holds[id];
  }
}
static void gain(uint32_t id) { present[id] = 1, remaining[id] = DURATION; }
static void lose(uint32_t id) { present[id] = 0; }
static void systems(void) { ent_frame(world, DT); }
static void readEntity(uint32_t id, float out[5]) {
  out[0] = ent_Character_Position_x(world)[id];
  out[1] = ent_Character_Position_y(world)[id];
  out[2] = ent_Character_Velocity_dx(world)[id];
  out[3] = ent_Character_Velocity_dy(world)[id];
  out[4] = present[id] ? remaining[id] : 0;
}
#endif

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr,
            "usage: %s <entities> <density> <churn> <frames> <reps>\n",
            argv[0]);
    return 2;
  }
  N = atoll(argv[1]);
  double density = atof(argv[2]), churn = atof(argv[3]);
  int frames = atoi(argv[4]), reps = atoi(argv[5]);
  int64_t initialHolders = (int64_t)(density * N);
  int64_t perSide = (int64_t)(churn * N / 2);
  if (perSide > initialHolders || perSide > N - initialHolders) {
    fprintf(stderr, "churn exceeds holders or non-holders\n");
    return 3;
  }

  // Initial holders: a random subset of the ids.
  uint32_t *order = malloc(N * 4);
  for (uint32_t i = 0; i < N; ++i)
    order[i] = i;
  for (int64_t i = N - 1; i > 0; --i) {
    uint32_t j = rng((uint32_t)i + 1), t = order[i];
    order[i] = order[j], order[j] = t;
  }
  uint8_t *holds = calloc(N, 1);
  for (int64_t i = 0; i < initialHolders; ++i)
    holds[order[i]] = 1;

  // Precompute every frame's changes from a variant-independent model of
  // who holds Status: pick perSide random holders to lose it and perSide
  // random others to gain it.
  uint32_t *in = malloc(N * 4), *out = malloc(N * 4), *pos = malloc(N * 4);
  int64_t nIn = 0, nOut = 0;
  for (uint32_t id = 0; id < N; ++id) {
    if (holds[id])
      pos[id] = (uint32_t)nIn, in[nIn++] = id;
    else
      pos[id] = (uint32_t)nOut, out[nOut++] = id;
  }
  uint32_t *loses = malloc((size_t)frames * perSide * 4 + 4);
  uint32_t *gains = malloc((size_t)frames * perSide * 4 + 4);
  for (int f = 0; f < frames; ++f) {
    for (int64_t k = 0; k < perSide; ++k) {
      uint32_t id = in[rng((uint32_t)nIn)];
      loses[f * perSide + k] = id;
      uint32_t last = in[--nIn];
      in[pos[id]] = last, pos[last] = pos[id];
      pos[id] = (uint32_t)nOut, out[nOut++] = id;
    }
    // An entity that just lost Status may gain it again in the same frame;
    // every variant applies all losses before the gains, so that is legal.
    for (int64_t k = 0; k < perSide; ++k) {
      uint32_t id = out[rng((uint32_t)nOut)];
      gains[f * perSide + k] = id;
      uint32_t last = out[--nOut];
      out[pos[id]] = last, pos[last] = pos[id];
      pos[id] = (uint32_t)nIn, in[nIn++] = id;
    }
  }

  uint64_t best = UINT64_MAX, bestChurn = 0, bestSystems = 0;
  double checksum = 0;
  for (int r = 0; r < reps; ++r) {
    allocated = 0;
    storageInit(holds);
    uint64_t churnTime = 0, systemsTime = 0;
    for (int f = 0; f < frames; ++f) {
      uint64_t t0 = now();
      for (int64_t k = 0; k < perSide; ++k)
        lose(loses[f * perSide + k]);
      for (int64_t k = 0; k < perSide; ++k)
        gain(gains[f * perSide + k]);
      uint64_t t1 = now();
      systems();
      uint64_t t2 = now();
      churnTime += t1 - t0, systemsTime += t2 - t1;
    }
    if (churnTime + systemsTime < best)
      best = churnTime + systemsTime, bestChurn = churnTime,
      bestSystems = systemsTime;
    if (r == 0) {
      float values[5];
      for (uint32_t id = 0; id < N; ++id) {
        readEntity(id, values);
        for (int k = 0; k < 5; ++k)
          checksum += values[k];
      }
    }
    // Storage is deliberately leaked per repetition: freeing and
    // reallocating would move columns between repetitions.
  }
  printf("ns_per_frame=%.1f churn_ns=%.1f systems_ns=%.1f bytes=%zu "
         "checksum=%.17g\n",
         (double)best / frames, (double)bestChurn / frames,
         (double)bestSystems / frames, allocated, checksum);
  return 0;
}
