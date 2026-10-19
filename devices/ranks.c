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
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int ent_rank = 0, ent_ranks = 1, ent_started = 0;
// Whether the distributed archetypes are placed (when `main` starts its
// first loop): until then every rank goes through every row, as one
// process, and what the ranks would combine or give each other is this
// rank's alone (a sum is its own part, a running sum starts at nought).
static int ent_distributed = 0;

void ent_ranks_distribute(void) { ent_distributed = 1; }

// (ENT_RANKS_LOG: the time each rank spent in each kind of call, which is
// where it waited for the others too, said as it ends.)
static const char *ent_kinds[] = {"bcast", "sum", "min", "max", "before", "release", "share_begin", "share_column", "exchange", "gather_column"};
static double ent_spent[10];
static int64_t ent_calls[10];

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

// (ENT_RANKS_LOG: how much memory the rank has, as it ends.)
static void ent_log_spent(void) {
  for (unsigned k = 0; k < sizeof ent_kinds / sizeof *ent_kinds; ++k)
    if (ent_calls[k])
      fprintf(stderr, "ent: rank %d: %-13s %8lld calls %9.3f s\n", ent_rank,
              ent_kinds[k], (long long)ent_calls[k], ent_spent[k]);
}

static void ent_log_memory(void) {
  FILE *status = fopen("/proc/self/status", "r");
  if (!status)
    return;
  char line[256];
  while (fgets(line, sizeof line, status))
    if (!strncmp(line, "VmRSS:", 6) || !strncmp(line, "VmHWM:", 6))
      fprintf(stderr, "ent: rank %d: %s", ent_rank, line);
  fclose(status);
}

void ent_ranks_end(void) {
  if (!ent_started)
    return;
  if (getenv("ENT_RANKS_LOG")) {
    ent_log_spent();
    ent_log_memory();
  }
  // (What rank 0 wrote is out before the other ranks are gone.)
  fflush(stdout);
  MPI_Finalize();
  ent_started = 0;
}

int32_t ent_ranks_rank(void) { return ent_rank; }
int32_t ent_ranks_count(void) { return ent_ranks; }

// `bytes` bytes at `data`, as rank 0 has them, on every rank. (In pieces
// of at most 1 GB: MPI counts in ints.)
static void ent_ranks_bcast_untimed(void *data, int64_t bytes) {
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
  int dropped;     // the other ranks' rows let go (ent_ranks_drop)
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
      ent_places[i] = (ent_placement){0, 0, 0, 0, 0};
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

// By its rows (an archetype not declared distributed that a `for` goes
// over with one that is): as many even stretches as there are ranks.
void ent_ranks_place_rows(int32_t archetype, int64_t count) {
  int32_t *rank_of = malloc(sizeof(int32_t) * (count > 0 ? count : 1));
  for (int64_t i = 0; i < count; ++i)
    rank_of[i] = (int32_t)((__int128)i * ent_ranks / count);
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

// This rank's stretch of the archetype's rows, of `count` it has: all of
// them before it is placed.
int64_t ent_ranks_first(int32_t archetype) {
  ent_placement *place = ent_place_of(archetype);
  return place->placed ? place->bounds[ent_rank] : 0;
}
int64_t ent_ranks_end_row(int32_t archetype, int64_t count) {
  ent_placement *place = ent_place_of(archetype);
  return place->placed ? place->bounds[ent_rank + 1] : count;
}

// Whether the other ranks' rows of the placed archetype are yet to be let
// go (1, once).
int32_t ent_ranks_dropping(int32_t archetype) {
  ent_placement *place = ent_place_of(archetype);
  if (!place->placed || place->dropped)
    return 0;
  place->dropped = 1;
  return 1;
}

// (The whole pages from `from` to `to` given back to the system: read
// again they are noughts.)
static void ent_let_go(char *from, char *to) {
  uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
  uintptr_t start = ((uintptr_t)from + page - 1) & ~(page - 1);
  uintptr_t end = (uintptr_t)to & ~(page - 1);
  if (start < end)
    madvise((void *)start, end - start, MADV_DONTNEED);
}

// The rows of a column (`each` bytes a row, `capacity` rows) of the
// placed archetype that other ranks have, let go: the rank keeps only the
// memory of its own. (Their row numbers stay as they are.)
void ent_ranks_drop(int32_t archetype, void *column, int64_t each,
                    int64_t capacity) {
  ent_placement *place = ent_place_of(archetype);
  char *rows = column;
  ent_let_go(rows, rows + place->bounds[ent_rank] * each);
  ent_let_go(rows + place->bounds[ent_rank + 1] * each, rows + capacity * each);
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

static int64_t ent_ranks_sum_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
  return all;
}
static int64_t ent_ranks_min_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_MIN, MPI_COMM_WORLD);
  return all;
}
static int64_t ent_ranks_max_untimed(int64_t mine) {
  if (!ent_distributed)
    return mine;
  int64_t all = 0;
  MPI_Allreduce(&mine, &all, 1, MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD);
  return all;
}
// What the ranks before this one added (nought on rank 0).
static int64_t ent_ranks_before_untimed(int64_t mine) {
  if (!ent_distributed)
    return 0;
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

static void ent_ranks_release_untimed(void) {
  int size = 0;
  if (ent_rank != 0) {
    fclose(stdout);
    stdout = ent_kept_stdout;
    size = (int)ent_captured_size;
  } else {
    fflush(stdout);
  }
  // (Before the placement every rank wrote all of it: rank 0's is it.)
  if (!ent_distributed) {
    free(ent_captured);
    ent_captured = 0;
    ent_captured_size = 0;
    return;
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

// ---------------------------------------------------------------------------
// Rows of a buffer that a `for` over distributed entities wrote: each rank
// wrote its entities' rows, from the least to the most it wrote (nothing
// where the least is above the most), and gives them to every other rank,
// each field's into the same rows there. Two ranks' stretches that overlap
// would give rows another rank wrote: not on many ranks yet.

static int *ent_share_counts = 0, *ent_share_starts = 0;
static int64_t *ent_share_lows = 0, *ent_share_highs = 0;
static int ent_share_any = 0;

static void ent_ranks_share_begin_untimed(int64_t low, int64_t high) {
  ent_share_any = 0;
  if (!ent_distributed)
    return;
  int64_t mine[2] = {low, high};
  int64_t *all = malloc(sizeof(int64_t) * 2 * ent_ranks);
  MPI_Allgather(mine, 2, MPI_INT64_T, all, 2, MPI_INT64_T, MPI_COMM_WORLD);
  ent_share_lows = realloc(ent_share_lows, sizeof(int64_t) * ent_ranks);
  ent_share_highs = realloc(ent_share_highs, sizeof(int64_t) * ent_ranks);
  ent_share_counts = realloc(ent_share_counts, sizeof(int) * ent_ranks);
  ent_share_starts = realloc(ent_share_starts, sizeof(int) * ent_ranks);
  ent_share_any = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    ent_share_lows[r] = all[2 * r];
    ent_share_highs[r] = all[2 * r + 1];
    ent_share_any |= ent_share_lows[r] <= ent_share_highs[r];
  }
  free(all);
  for (int a = 0; a < ent_ranks; ++a)
    for (int b = a + 1; b < ent_ranks; ++b)
      if (ent_share_lows[a] <= ent_share_highs[a] &&
          ent_share_lows[b] <= ent_share_highs[b] &&
          ent_share_lows[a] <= ent_share_highs[b] &&
          ent_share_lows[b] <= ent_share_highs[a])
        ent_ranks_stop("two ranks' entities wrote rows of a buffer between "
                       "each other's (their windows or places are not "
                       "apart by rank): not on many ranks yet");
}

// One column of the buffer, `each` bytes a row, from `column`.
static void ent_ranks_share_column_untimed(void *column, int64_t each) {
  if (!ent_share_any)
    return;
  for (int r = 0; r < ent_ranks; ++r) {
    int64_t rows = ent_share_lows[r] <= ent_share_highs[r]
                       ? ent_share_highs[r] - ent_share_lows[r] + 1
                       : 0;
    int64_t bytes = rows * each, start = rows ? ent_share_lows[r] * each : 0;
    if (bytes > 0x7fffffff || start > 0x7fffffff)
      ent_ranks_stop("a rank wrote more than 2 GB of a buffer's column, or "
                     "rows past 2 GB: not on many ranks yet");
    ent_share_counts[r] = (int)bytes;
    ent_share_starts[r] = (int)start;
  }
  MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_BYTE, column, ent_share_counts,
                 ent_share_starts, MPI_BYTE, MPI_COMM_WORLD);
}

// ---------------------------------------------------------------------------
// Sends of a `for` over distributed entities to entities another rank has
// (or every rank: of an archetype that is not distributed): each posted to
// those ranks, on a channel of the program's (one for each field and rule
// it combines into), and given to them when the `for` ends, where each is
// combined into the entity's field. Only whole numbers added, or kept the
// least or the most, which come to the same in any order.

typedef struct {
  int64_t *words; // channel, id, value
  int64_t count, room;
} ent_posts;

static ent_posts *ent_outbox = 0;
static int64_t *ent_inbox = 0;
static int64_t ent_inbox_count = 0;
// (For each channel, where its sends start in the inbox, sorted by it.)
static int64_t *ent_channel_starts = 0;
static int32_t ent_channels = 0;

static void ent_post_to(int32_t rank, int32_t channel, int64_t id,
                        int64_t value) {
  if (!ent_outbox)
    ent_outbox = calloc(ent_ranks, sizeof(ent_posts));
  ent_posts *box = &ent_outbox[rank];
  if (box->count + 3 > box->room) {
    box->room = box->room ? box->room * 2 : 3 * 1024;
    box->words = realloc(box->words, sizeof(int64_t) * box->room);
  }
  box->words[box->count++] = channel;
  box->words[box->count++] = id;
  box->words[box->count++] = value;
  if (channel >= ent_channels)
    ent_channels = channel + 1;
}

void ent_ranks_post(int32_t channel, int32_t archetype, int64_t row,
                    int64_t id, int64_t value, int32_t all) {
  // (Before the placement every rank sends all of it: to itself.)
  if (!ent_distributed) {
    ent_post_to(ent_rank, channel, id, value);
    return;
  }
  if (all) {
    for (int r = 0; r < ent_ranks; ++r)
      ent_post_to(r, channel, id, value);
    return;
  }
  ent_post_to(ent_ranks_owner(archetype, row), channel, id, value);
}

// Every rank's posts to every rank, sorted by channel (in the order of the
// ranks they came from, each's in its order).
static void ent_ranks_exchange_untimed(int32_t channels) {
  if (!ent_outbox)
    ent_outbox = calloc(ent_ranks, sizeof(ent_posts));
  int *sent = malloc(sizeof(int) * ent_ranks), *got = malloc(sizeof(int) * ent_ranks);
  int *sent_at = malloc(sizeof(int) * ent_ranks), *got_at = malloc(sizeof(int) * ent_ranks);
  int64_t total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    if (ent_outbox[r].count > 0x7fffffff || total > 0x7fffffff)
      ent_ranks_stop("a rank sent more than 2^31 words to the others in one "
                     "`for`: not on many ranks yet");
    sent[r] = (int)ent_outbox[r].count;
    total += sent[r];
  }
  int64_t *out = malloc(sizeof(int64_t) * (total > 0 ? total : 1));
  total = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    sent_at[r] = (int)total;
    for (int64_t i = 0; i < ent_outbox[r].count; ++i)
      out[total++] = ent_outbox[r].words[i];
    ent_outbox[r].count = 0;
  }
  MPI_Alltoall(sent, 1, MPI_INT, got, 1, MPI_INT, MPI_COMM_WORLD);
  int64_t incoming = 0;
  for (int r = 0; r < ent_ranks; ++r) {
    got_at[r] = (int)incoming;
    incoming += got[r];
  }
  int64_t *in = malloc(sizeof(int64_t) * (incoming > 0 ? incoming : 1));
  MPI_Alltoallv(out, sent, sent_at, MPI_INT64_T, in, got, got_at, MPI_INT64_T,
                MPI_COMM_WORLD);
  free(out);
  free(sent);
  free(got);
  free(sent_at);
  free(got_at);
  // (By channel: counted, then put in place.)
  if (channels > ent_channels)
    ent_channels = channels;
  ent_channel_starts =
      realloc(ent_channel_starts, sizeof(int64_t) * (ent_channels + 1));
  for (int32_t c = 0; c <= ent_channels; ++c)
    ent_channel_starts[c] = 0;
  ent_inbox_count = incoming / 3;
  for (int64_t i = 0; i < ent_inbox_count; ++i)
    ++ent_channel_starts[in[3 * i] + 1];
  for (int32_t c = 0; c < ent_channels; ++c)
    ent_channel_starts[c + 1] += ent_channel_starts[c];
  int64_t *place = malloc(sizeof(int64_t) * (ent_channels + 1));
  for (int32_t c = 0; c <= ent_channels; ++c)
    place[c] = ent_channel_starts[c];
  ent_inbox = realloc(ent_inbox, sizeof(int64_t) * 2 * (ent_inbox_count + 1));
  for (int64_t i = 0; i < ent_inbox_count; ++i) {
    int64_t at = place[in[3 * i]]++;
    ent_inbox[2 * at] = in[3 * i + 1];
    ent_inbox[2 * at + 1] = in[3 * i + 2];
  }
  free(place);
  free(in);
}

// How many sends this rank was given on the channel, and the `i`th one's
// entity and value.
int64_t ent_ranks_got(int32_t channel) {
  if (channel >= ent_channels)
    return 0;
  return ent_channel_starts[channel + 1] - ent_channel_starts[channel];
}
int64_t ent_ranks_got_id(int32_t channel, int64_t i) {
  return ent_inbox[2 * (ent_channel_starts[channel] + i)];
}
int64_t ent_ranks_got_value(int32_t channel, int64_t i) {
  return ent_inbox[2 * (ent_channel_starts[channel] + i) + 1];
}

// A send that cannot go to the other ranks yet (a fraction, `=`, or to a
// field whose changes are events) to an entity they have too.
void ent_ranks_cannot_send(int32_t line) {
  char why[256];
  snprintf(why, sizeof why,
           "a `for` over distributed entities sends a value that is not a "
           "whole number added, or kept the least or the most, to an entity "
           "another rank has or every rank has (line %d): not on many ranks "
           "yet",
           line);
  ent_ranks_stop(why);
}

// ---------------------------------------------------------------------------
// A column of a placed archetype (`each` bytes a row) that a `for` over
// distributed entities looks up in entities of any rank: each rank's
// stretch given to every other rank, so that every rank has all of it (the
// ghosts of the other ranks' entities) until the `for` ends.
static void ent_ranks_gather_column_untimed(int32_t archetype, void *column, int64_t each) {
  if (!ent_distributed)
    return;
  ent_placement *place = ent_place_of(archetype);
  int *counts = malloc(sizeof(int) * ent_ranks);
  int *starts = malloc(sizeof(int) * ent_ranks);
  for (int r = 0; r < ent_ranks; ++r) {
    int64_t start = place->bounds[r] * each;
    int64_t bytes = (place->bounds[r + 1] - place->bounds[r]) * each;
    if (start > 0x7fffffff || bytes > 0x7fffffff)
      ent_ranks_stop("a column looked up across ranks is past 2 GB: not on "
                     "many ranks yet");
    counts[r] = (int)bytes;
    starts[r] = (int)start;
  }
  MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_BYTE, column, counts, starts, MPI_BYTE,
                 MPI_COMM_WORLD);
  free(counts);
  free(starts);
}

// A child of a tree that a `for` over distributed entities goes along
// (row `row` of `archetype`) and its parent (`row` of `home`): the
// same rank's, else the program stops when they are placed.
void ent_ranks_same_rank(int32_t archetype, int64_t row, int32_t home,
                         int64_t home_row) {
  int32_t child = ent_ranks_owner(archetype, row);
  int32_t parent = ent_ranks_owner(home, home_row);
  if (child != parent) {
    char why[256];
    snprintf(why, sizeof why,
             "a child of a tree (row %lld of archetype %d) is placed on rank "
             "%d, its parent (row %lld of archetype %d) on rank %d: a `for` "
             "along the tree needs them on one rank (distribute the child "
             "by its parent, or both by one number)",
             (long long)row, archetype, child, (long long)home_row, home,
             parent);
    ent_ranks_stop(why);
  }
}

// A connect or disconnect in a `for` over distributed entities, after
// they were placed: the ranks' edges would come apart.
void ent_ranks_cannot_connect(int32_t line) {
  if (!ent_distributed)
    return;
  char why[200];
  snprintf(why, sizeof why,
           "a `for` over distributed entities connects or disconnects edges "
           "(line %d): not on many ranks yet",
           line);
  ent_ranks_stop(why);
}

// ---------------------------------------------------------------------------
// The calls the compiler makes, timed (see ent_spent).

void ent_ranks_bcast(void *data, int64_t bytes) {
  double start = MPI_Wtime();
  ent_ranks_bcast_untimed(data, bytes);
  ent_spent[0] += MPI_Wtime() - start;
  ++ent_calls[0];
}

int64_t ent_ranks_sum(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_sum_untimed(mine);
  ent_spent[1] += MPI_Wtime() - start;
  ++ent_calls[1];
  return result;
}

int64_t ent_ranks_min(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_min_untimed(mine);
  ent_spent[2] += MPI_Wtime() - start;
  ++ent_calls[2];
  return result;
}

int64_t ent_ranks_max(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_max_untimed(mine);
  ent_spent[3] += MPI_Wtime() - start;
  ++ent_calls[3];
  return result;
}

int64_t ent_ranks_before(int64_t mine) {
  double start = MPI_Wtime();
  int64_t result = ent_ranks_before_untimed(mine);
  ent_spent[4] += MPI_Wtime() - start;
  ++ent_calls[4];
  return result;
}

void ent_ranks_release(void) {
  double start = MPI_Wtime();
  ent_ranks_release_untimed();
  ent_spent[5] += MPI_Wtime() - start;
  ++ent_calls[5];
}

void ent_ranks_share_begin(int64_t low, int64_t high) {
  double start = MPI_Wtime();
  ent_ranks_share_begin_untimed(low, high);
  ent_spent[6] += MPI_Wtime() - start;
  ++ent_calls[6];
}

void ent_ranks_share_column(void *column, int64_t each) {
  double start = MPI_Wtime();
  ent_ranks_share_column_untimed(column, each);
  ent_spent[7] += MPI_Wtime() - start;
  ++ent_calls[7];
}

void ent_ranks_exchange(int32_t channels) {
  double start = MPI_Wtime();
  ent_ranks_exchange_untimed(channels);
  ent_spent[8] += MPI_Wtime() - start;
  ++ent_calls[8];
}

void ent_ranks_gather_column(int32_t archetype, void *column, int64_t each) {
  double start = MPI_Wtime();
  ent_ranks_gather_column_untimed(archetype, column, each);
  ent_spent[9] += MPI_Wtime() - start;
  ++ent_calls[9];
}
