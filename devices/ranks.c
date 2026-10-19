// A program on many ranks (`tools/ent build --ranks`, docs/ranks.md):
// what the compiler calls, which alone calls MPI.
//
// The same program runs as many processes, the ranks, each with a world
// of its own; `mpirun -n 4 program` starts four. What a device does (an
// extern proc: the console, files, the clock) rank 0 does, and what it
// gives back every rank is given (`ent_ranks_bcast`), so that every rank
// goes on alike and the program says what it says on one process.
// (open_memstream.)
#define _GNU_SOURCE
#include <execinfo.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int ent_rank = 0, ent_ranks = 1, ent_started = 0;

void ent_ranks_start(void) {
  if (ent_started)
    return;
  int ready = 0;
  MPI_Initialized(&ready);
  if (!ready && MPI_Init(NULL, NULL) != MPI_SUCCESS) {
    fprintf(stderr, "ent: MPI did not start\n");
    exit(1);
  }
  MPI_Comm_rank(MPI_COMM_WORLD, &ent_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ent_ranks);
  ent_started = 1;
}

void ent_ranks_end(void) {
  if (!ent_started)
    return;
  // (What rank 0 wrote is out before the other ranks are gone.)
  fflush(stdout);
  MPI_Finalize();
  ent_started = 0;
}

int32_t ent_ranks_rank(void) { return ent_rank; }
int32_t ent_ranks_count(void) { return ent_ranks; }

// `bytes` bytes at `data`, as rank 0 has them, on every rank. (In pieces
// of at most 1 GB: MPI counts in ints.)
void ent_ranks_bcast(void *data, int64_t bytes) {
  char *at = (char *)data;
  while (bytes > 0) {
    int piece = bytes > (1 << 30) ? (1 << 30) : (int)bytes;
    MPI_Bcast(at, piece, MPI_BYTE, 0, MPI_COMM_WORLD);
    at += piece;
    bytes -= piece;
  }
}

// ---------------------------------------------------------------------------
// Placement. The entities of a distributed archetype (`distribute Colony
// by number`) are every rank's, as the program spawned them on every rank;
// each rank goes through its own stretch of the rows, `bounds[rank]` to
// `bounds[rank + 1]`, and the other rows are not its to read or write. The
// stretches are worked out alike on every rank, from what every rank has.

typedef struct {
  int placed;
  int64_t count;
  int64_t *bounds; // ranks + 1
  int32_t *noted;  // while placing by an entity: each row's rank
} ent_placement;

static ent_placement *ent_places = 0;
static int32_t ent_places_known = 0;

static void ent_ranks_stop(const char *why) {
  fprintf(stderr, "ent: %s\n", why);
  // (ENT_RANKS_LOG: where it was, by the functions' names.)
  if (getenv("ENT_RANKS_LOG")) {
    void *frames[24];
    backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
  }
  fflush(stderr);
  MPI_Abort(MPI_COMM_WORLD, 1);
  exit(1);
}

static ent_placement *ent_place_of(int32_t archetype) {
  if (archetype >= ent_places_known) {
    int32_t more = archetype + 16;
    ent_places = realloc(ent_places, sizeof(ent_placement) * more);
    for (int32_t i = ent_places_known; i < more; ++i)
      ent_places[i] = (ent_placement){0, 0, 0, 0};
    ent_places_known = more;
  }
  return &ent_places[archetype];
}

// Whether the archetype is to be placed (1), or is placed with these many
// entities already (0). Entities spawned into it, or gone from it, after
// it was placed stop the program: not on many ranks yet.
int32_t ent_ranks_unplaced(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  if (!place->placed)
    return 1;
  if (place->count != count)
    ent_ranks_stop("entities were spawned into, or are gone from, a "
                   "distributed archetype after its entities were placed: "
                   "not on many ranks yet");
  return 0;
}

// (Stretches from each row's rank, which go up with the rows.)
static void ent_settle(ent_placement *place, int64_t count,
                       const int32_t *rank_of) {
  place->bounds = realloc(place->bounds, sizeof(int64_t) * (ent_ranks + 1));
  int64_t row = 0;
  for (int r = 0; r <= ent_ranks; ++r) {
    while (row < count && rank_of[row] < r)
      ++row;
    place->bounds[r] = row;
  }
  for (int64_t i = 1; i < count; ++i)
    if (rank_of[i] < rank_of[i - 1])
      ent_ranks_stop("the rows of a distributed archetype are not in the "
                     "order of what it is distributed by: not on many ranks "
                     "yet");
  place->count = count;
  place->placed = 1;
  // (ENT_RANKS_LOG: each rank's stretch, as it is placed.)
  if (getenv("ENT_RANKS_LOG"))
    fprintf(stderr, "ent: rank %d of %d: rows %lld to %lld of %lld\n",
            ent_rank, ent_ranks, (long long)place->bounds[ent_rank],
            (long long)place->bounds[ent_rank + 1], (long long)count);
}

static int64_t ent_value(const void *values, int32_t bits, int32_t is_signed,
                         int64_t row) {
  switch (bits) {
  case 8:
    return is_signed ? ((const int8_t *)values)[row]
                     : ((const uint8_t *)values)[row];
  case 16:
    return is_signed ? ((const int16_t *)values)[row]
                     : ((const uint16_t *)values)[row];
  case 32:
    return is_signed ? ((const int32_t *)values)[row]
                     : ((const uint32_t *)values)[row];
  default:
    return ((const int64_t *)values)[row];
  }
}

// By a whole number: its values from the least to the most cut into as
// many equal ranges as there are ranks.
void ent_ranks_place_range(int32_t archetype, int64_t count,
                           const void *values, int32_t bits,
                           int32_t is_signed) {
  int32_t *rank_of = malloc(sizeof(int32_t) * (count > 0 ? count : 1));
  int64_t least = 0, most = 0;
  for (int64_t i = 0; i < count; ++i) {
    int64_t v = ent_value(values, bits, is_signed, i);
    if (i == 0 || v < least)
      least = v;
    if (i == 0 || v > most)
      most = v;
  }
  __int128 span = (__int128)most - least + 1;
  for (int64_t i = 0; i < count; ++i)
    rank_of[i] = (int32_t)(((__int128)ent_value(values, bits, is_signed, i) -
                            least) * ent_ranks / span);
  ent_settle(ent_place_of(archetype), count, rank_of);
  free(rank_of);
}

// By an entity: each row with the rank of the entity it names, noted one
// by one, then settled.
void ent_ranks_note(int32_t archetype, int64_t count, int64_t row,
                    int32_t rank) {
  ent_placement *place = ent_place_of(archetype);
  if (row == 0)
    place->noted = realloc(place->noted,
                           sizeof(int32_t) * (count > 0 ? count : 1));
  place->noted[row] = rank;
}

void ent_ranks_settle(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  if (count == 0)
    place->noted = realloc(place->noted, sizeof(int32_t));
  ent_settle(place, count, place->noted);
  free(place->noted);
  place->noted = 0;
}

// The rank whose row it is; -1 for an archetype that is not distributed,
// which every rank has.
int32_t ent_ranks_owner(int32_t archetype, int64_t row) {
  if (archetype >= ent_places_known || !ent_places[archetype].placed)
    return -1;
  const int64_t *bounds = ent_places[archetype].bounds;
  int32_t lo = 0, hi = ent_ranks - 1;
  while (lo < hi) {
    int32_t mid = (lo + hi + 1) / 2;
    if (bounds[mid] <= row)
      lo = mid;
    else
      hi = mid - 1;
  }
  return lo;
}

// Placed by an entity whose archetype is not distributed: nothing to
// place it with.
int32_t ent_ranks_owner_of_home(int32_t archetype, int64_t row) {
  int32_t rank = ent_ranks_owner(archetype, row);
  if (rank < 0)
    ent_ranks_stop("an entity is distributed by an entity that every rank "
                   "has (not distributed, or not placed yet): there is "
                   "nothing to place it with");
  return rank;
}

int64_t ent_ranks_first(int32_t archetype) {
  return ent_place_of(archetype)->bounds[ent_rank];
}
int64_t ent_ranks_end_row(int32_t archetype) {
  return ent_place_of(archetype)->bounds[ent_rank + 1];
}

// A row of a distributed archetype that another rank has is not read or
// written here: not on many ranks yet.
void ent_ranks_check_own(int32_t archetype, int64_t row, int32_t line) {
  int32_t rank = ent_ranks_owner(archetype, row);
  if (rank >= 0 && rank != ent_rank) {
    char why[256];
    snprintf(why, sizeof why,
             "rank %d reads or sends to an entity that rank %d has (row %lld "
             "of archetype %d, line %d): not on many ranks yet",
             ent_rank, rank, (long long)row, archetype, line);
    ent_ranks_stop(why);
  }
}

// ---------------------------------------------------------------------------
// What the ranks' parts of a `for` come to.

int64_t ent_ranks_sum(int64_t mine) {
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
  return all;
}
int64_t ent_ranks_min(int64_t mine) {
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_MIN, MPI_COMM_WORLD);
  return all;
}
int64_t ent_ranks_max(int64_t mine) {
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD);
  return all;
}
// What the ranks before this one added (nought on rank 0).
int64_t ent_ranks_before(int64_t mine) {
  int64_t before = 0;
  MPI_Exscan(&mine, &before, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
  return ent_rank == 0 ? 0 : before;
}

// ---------------------------------------------------------------------------
// What a `for` over a distributed archetype writes to standard output: each
// rank writes what its rows write, the ranks after 0 into memory, which
// rank 0 then writes after its own, rank by rank: the order of the rows.

static FILE *ent_kept_stdout = 0;
static char *ent_captured = 0;
static size_t ent_captured_size = 0;

void ent_ranks_capture(void) {
  fflush(stdout);
  if (ent_rank == 0)
    return;
  ent_kept_stdout = stdout;
  stdout = open_memstream(&ent_captured, &ent_captured_size);
}

void ent_ranks_release(void) {
  int size = 0;
  if (ent_rank != 0) {
    fclose(stdout);
    stdout = ent_kept_stdout;
    size = (int)ent_captured_size;
  } else {
    fflush(stdout);
  }
  int *sizes = ent_rank == 0 ? malloc(sizeof(int) * ent_ranks) : 0;
  MPI_Gather(&size, 1, MPI_INT, sizes, 1, MPI_INT, 0, MPI_COMM_WORLD);
  int *starts = 0;
  char *all = 0;
  if (ent_rank == 0) {
    starts = malloc(sizeof(int) * ent_ranks);
    int total = 0;
    for (int r = 0; r < ent_ranks; ++r) {
      starts[r] = total;
      total += sizes[r];
    }
    all = malloc(total > 0 ? total : 1);
  }
  MPI_Gatherv(ent_rank == 0 ? MPI_IN_PLACE : ent_captured, size, MPI_BYTE, all,
              sizes, starts, MPI_BYTE, 0, MPI_COMM_WORLD);
  if (ent_rank == 0) {
    for (int r = 1; r < ent_ranks; ++r)
      fwrite(all + starts[r], 1, sizes[r], stdout);
    fflush(stdout);
    free(sizes);
    free(starts);
    free(all);
  }
  free(ent_captured);
  ent_captured = 0;
  ent_captured_size = 0;
}
