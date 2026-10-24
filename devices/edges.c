// A relation's edges kept in their order where few of them change: the
// generated sort (lib/Ent/Transforms/LowerToLoops.cpp, emitSortFunction)
// calls these for a relation that is connected or disconnected after the
// world is made (WorldRelation::mended).
//
// After a sort the table holds each entity's edges, in the order they were
// connected, in a run of its own with room after them: `sorted[k]` is
// where key k's run starts, `ends[k]` where its edges end, and up to
// `sorted[k + 1]` there is room (dead marked 2, as edges never are). A
// relation read both ways has the index by target likewise: each target's
// run of positions (`index`, `index_ends`), in table order, with the copies
// of the edges' ends and fields in that order (the scratch columns), and
// for every edge where its position is in the index (`slot`).
//
// ent_edges_mend takes out every edge marked dead (1) in the runs, the
// edges after it moved down by one, and moves every edge connected since
// (after the runs, as connect appends) to the end of its run, its place in
// the index by target among the others in table order: the order a sort
// anew would give. Where a run has no room, or more changed than a
// sixteenth of the edges since the last sort anew, it says so and leaves
// the table as a sort anew can take it (what it moved marked dead).
// ent_edges_spread, after a sort anew (the edges side by side), gives every
// run room again where the last mending found a run without, and none
// where too much had changed (as when the table is first made); a run that
// is full when an edge comes takes room from the nearest one with some,
// the runs between moved over by one. ent_edges_dense, after the sort of a
// partition, gives none; ent_edges_squeeze takes it out where the table is
// full while edges are connected.
//
// With ENT_EDGES_ANEW=1 in the environment it is always sorted anew.
//
// The spec: what the generated code knows of the relation, byte offsets in
// the world's arena and sizes (see SPEC_* below).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  SPEC_FLAGS,        // 1: an index by target; 2: the table is by target;
                     // 4: the index's positions (and slots) are kept
  SPEC_KEYS,         // entity keys
  SPEC_CAPACITY,     // edges the table holds
  SPEC_OFFSET_BYTES, // 4 or 8
  SPEC_ID_BYTES,     // 4 or 8
  SPEC_KEY_MASK,     // an id's key: its bits and these
  SPEC_COUNT,        // i64: edges in the table, with those connected since
  SPEC_SORTED,       // keys + 1 offsets
  SPEC_ENDS,         // keys offsets
  SPEC_SOURCE,
  SPEC_TARGET,
  SPEC_DEAD,
  SPEC_INDEX,       // keys + 1 offsets
  SPEC_INDEX_ENDS,  // keys offsets
  SPEC_INDEX_EDGES, // capacity offsets
  SPEC_SLOT,        // capacity offsets
  SPEC_SOURCE_COPY, // capacity ids
  SPEC_TARGET_COPY, // capacity ids
  SPEC_MENDS,       // i64s: edges changed since the last sort anew, and
                    // why the last mending did not mend (WHY_*)
  SPEC_FIELDS,      // how many; then for each its offset, bytes and copy
  SPEC_FIELD0,
};

// Why it was sorted anew: too much changed (then the next sort makes no
// room in the runs, which the next change would not fit either), or a run
// had no room; or why it is to be: room was taken from far runs
// (crowded), which a sort that gives room again spares the next ones.
enum { WHY_NONE, WHY_MANY, WHY_ROOM, WHY_CROWDED };

typedef struct {
  char *arena;
  const int64_t *spec;
  int64_t keys, capacity, offset_bytes, id_bytes;
  uint64_t mask;
  int index, by_target, positions;
} Edges;

static int64_t get_at(const char *base, int64_t bytes, int64_t i) {
  if (bytes == 4) {
    uint32_t v;
    memcpy(&v, base + 4 * i, 4);
    return (int64_t)v;
  }
  int64_t v;
  memcpy(&v, base + 8 * i, 8);
  return v;
}

static void set_at(char *base, int64_t bytes, int64_t i, int64_t value) {
  if (bytes == 4) {
    uint32_t v = (uint32_t)value;
    memcpy(base + 4 * i, &v, 4);
  } else
    memcpy(base + 8 * i, &value, 8);
}

static char *at(const Edges *e, int which) { return e->arena + e->spec[which]; }
static int64_t off(const Edges *e, int which, int64_t i) {
  return get_at(at(e, which), e->offset_bytes, i);
}
static void set_off(const Edges *e, int which, int64_t i, int64_t v) {
  set_at(at(e, which), e->offset_bytes, i, v);
}
static int64_t key_of_id(const Edges *e, int64_t id) {
  return (int64_t)((uint64_t)id & e->mask);
}
static int64_t id_at(const Edges *e, int which, int64_t i) {
  return get_at(at(e, which), e->id_bytes, i);
}
// The key an edge of the table is in a run by, and its target's.
static int64_t run_key(const Edges *e, int64_t p) {
  return key_of_id(e, id_at(e, e->by_target ? SPEC_TARGET : SPEC_SOURCE, p));
}
static int64_t target_key(const Edges *e, int64_t p) {
  return key_of_id(e, id_at(e, SPEC_TARGET, p));
}

static Edges edges_of(char *arena, const int64_t *spec) {
  Edges e = {arena,
             spec,
             spec[SPEC_KEYS],
             spec[SPEC_CAPACITY],
             spec[SPEC_OFFSET_BYTES],
             spec[SPEC_ID_BYTES],
             (uint64_t)spec[SPEC_KEY_MASK],
             (int)(spec[SPEC_FLAGS] & 1),
             (int)((spec[SPEC_FLAGS] >> 1) & 1),
             (int)((spec[SPEC_FLAGS] & 5) == 5)};
  return e;
}

// The table's columns (ends, fields, the dead marks and the slots): `n`
// elements of each from `from` to `to`, as memmove moves them.
static void move_edges(const Edges *e, int64_t from, int64_t to, int64_t n) {
  if (n <= 0 || from == to)
    return;
  const int64_t *spec = e->spec;
  memmove(at(e, SPEC_SOURCE) + to * e->id_bytes,
          at(e, SPEC_SOURCE) + from * e->id_bytes, n * e->id_bytes);
  memmove(at(e, SPEC_TARGET) + to * e->id_bytes,
          at(e, SPEC_TARGET) + from * e->id_bytes, n * e->id_bytes);
  for (int64_t f = 0; f < spec[SPEC_FIELDS]; f++) {
    int64_t offset = spec[SPEC_FIELD0 + 3 * f],
            bytes = spec[SPEC_FIELD0 + 3 * f + 1];
    memmove(e->arena + offset + to * bytes, e->arena + offset + from * bytes,
            n * bytes);
  }
  memmove(at(e, SPEC_DEAD) + to, at(e, SPEC_DEAD) + from, n);
  if (e->positions)
    memmove(at(e, SPEC_SLOT) + to * e->offset_bytes,
            at(e, SPEC_SLOT) + from * e->offset_bytes, n * e->offset_bytes);
}

// The index's positions and copies, likewise.
static void move_index(const Edges *e, int64_t from, int64_t to, int64_t n) {
  if (n <= 0 || from == to)
    return;
  const int64_t *spec = e->spec;
  if (e->positions)
    memmove(at(e, SPEC_INDEX_EDGES) + to * e->offset_bytes,
            at(e, SPEC_INDEX_EDGES) + from * e->offset_bytes,
            n * e->offset_bytes);
  memmove(at(e, SPEC_SOURCE_COPY) + to * e->id_bytes,
          at(e, SPEC_SOURCE_COPY) + from * e->id_bytes, n * e->id_bytes);
  memmove(at(e, SPEC_TARGET_COPY) + to * e->id_bytes,
          at(e, SPEC_TARGET_COPY) + from * e->id_bytes, n * e->id_bytes);
  for (int64_t f = 0; f < spec[SPEC_FIELDS]; f++) {
    int64_t copy = spec[SPEC_FIELD0 + 3 * f + 2],
            bytes = spec[SPEC_FIELD0 + 3 * f + 1];
    if (copy)
      memmove(e->arena + copy + to * bytes, e->arena + copy + from * bytes,
              n * bytes);
  }
}

// The index's entry `j` made of the table's edge at `p`.
static void copy_into_index(const Edges *e, int64_t p, int64_t j) {
  const int64_t *spec = e->spec;
  if (e->positions)
    set_off(e, SPEC_INDEX_EDGES, j, p);
  memcpy(at(e, SPEC_SOURCE_COPY) + j * e->id_bytes,
         at(e, SPEC_SOURCE) + p * e->id_bytes, e->id_bytes);
  memcpy(at(e, SPEC_TARGET_COPY) + j * e->id_bytes,
         at(e, SPEC_TARGET) + p * e->id_bytes, e->id_bytes);
  for (int64_t f = 0; f < spec[SPEC_FIELDS]; f++) {
    int64_t offset = spec[SPEC_FIELD0 + 3 * f],
            bytes = spec[SPEC_FIELD0 + 3 * f + 1],
            copy = spec[SPEC_FIELD0 + 3 * f + 2];
    if (copy)
      memcpy(e->arena + copy + j * bytes, e->arena + offset + p * bytes, bytes);
  }
}

// Every edge's slot from the index's entries [from, to).
static void slots_from(const Edges *e, int64_t from, int64_t to) {
  if (!e->positions)
    return;
  for (int64_t j = from; j < to; j++)
    set_off(e, SPEC_SLOT, off(e, SPEC_INDEX_EDGES, j), j);
}

void ent_edges_dense(char *arena, const int64_t *spec) {
  Edges e = edges_of(arena, spec);
  for (int64_t k = 0; k < e.keys; k++)
    set_off(&e, SPEC_ENDS, k, off(&e, SPEC_SORTED, k + 1));
  if (e.index) {
    for (int64_t k = 0; k < e.keys; k++)
      set_off(&e, SPEC_INDEX_ENDS, k, off(&e, SPEC_INDEX, k + 1));
    slots_from(&e, 0, off(&e, SPEC_INDEX, e.keys));
  }
  *(int64_t *)(arena + spec[SPEC_MENDS]) = 0;
}

// New starts for runs of `lengths` (keys of them) with room after each:
// one for every key up to `top` (those past it have no entity with an
// edge) and an eighth of its length, all of it no more than `room` (each
// share made smaller where it would be).
static int64_t *spread_starts(const Edges *e, const int64_t *lengths,
                              int64_t top, int64_t room) {
  int64_t want = top;
  for (int64_t k = 0; k < e->keys; k++)
    want += lengths[k] / 8;
  int64_t *starts = malloc(sizeof(int64_t) * (e->keys + 1));
  int64_t at_ = 0;
  for (int64_t k = 0; k < e->keys; k++) {
    starts[k] = at_;
    int64_t more = (k < top ? 1 : 0) + lengths[k] / 8;
    if (want > room)
      more = (int64_t)((__int128)more * room / want);
    at_ += lengths[k] + more;
  }
  starts[e->keys] = at_;
  return starts;
}

// One more than the greatest key with a run of `lengths` not empty.
static int64_t top_key(const Edges *e, const int64_t *lengths, int64_t top) {
  for (int64_t k = e->keys - 1; k >= top; k--)
    if (lengths[k])
      return k + 1;
  return top;
}

void ent_edges_spread(char *arena, const int64_t *spec) {
  Edges e = edges_of(arena, spec);
  int64_t *count = (int64_t *)(arena + spec[SPEC_COUNT]);
  int64_t total = *count;
  // At most half of what the table has left is room in the runs; the
  // other half for what is connected before the next mending.
  int64_t room = (e.capacity - total) / 2;
  // (None where it was sorted anew for how much changed.)
  int64_t *mends = (int64_t *)(arena + spec[SPEC_MENDS]);
  if (mends[1] == WHY_MANY) {
    ent_edges_dense(arena, spec);
    return;
  }
  int64_t *lengths = malloc(sizeof(int64_t) * e.keys);
  for (int64_t k = 0; k < e.keys; k++)
    lengths[k] = off(&e, SPEC_SORTED, k + 1) - off(&e, SPEC_SORTED, k);
  int64_t top = top_key(&e, lengths, 0);
  int64_t *index_lengths = NULL, *index_starts = NULL;
  if (e.index) {
    slots_from(&e, 0, off(&e, SPEC_INDEX, e.keys));
    index_lengths = malloc(sizeof(int64_t) * e.keys);
    for (int64_t k = 0; k < e.keys; k++)
      index_lengths[k] = off(&e, SPEC_INDEX, k + 1) - off(&e, SPEC_INDEX, k);
    top = top_key(&e, index_lengths, top);
  }
  int64_t *starts = spread_starts(&e, lengths, top, room);
  // (The index as long as the table, and the table as the index: a GPU
  // copies as many of each as the table counts.)
  if (e.index) {
    index_starts = spread_starts(&e, index_lengths, top, room);
    int64_t longest = starts[e.keys] > index_starts[e.keys]
                          ? starts[e.keys]
                          : index_starts[e.keys];
    starts[e.keys] = index_starts[e.keys] = longest;
  }
  // From the last run to the first: each to its place, its room dead.
  char *dead = at(&e, SPEC_DEAD);
  for (int64_t k = e.keys - 1; k >= 0; k--) {
    move_edges(&e, off(&e, SPEC_SORTED, k), starts[k], lengths[k]);
    memset(dead + starts[k] + lengths[k], 2,
           starts[k + 1] - starts[k] - lengths[k]);
  }
  for (int64_t k = 0; k < e.keys; k++) {
    set_off(&e, SPEC_SORTED, k, starts[k]);
    set_off(&e, SPEC_ENDS, k, starts[k] + lengths[k]);
  }
  set_off(&e, SPEC_SORTED, e.keys, starts[e.keys]);
  *count = starts[e.keys];
  if (e.index) {
    // The index's positions are the edges' new ones; then its runs too.
    if (e.positions)
      for (int64_t k = 0; k < e.keys; k++)
        for (int64_t p = starts[k]; p < starts[k] + lengths[k]; p++)
          set_off(&e, SPEC_INDEX_EDGES, off(&e, SPEC_SLOT, p), p);
    for (int64_t k = e.keys - 1; k >= 0; k--)
      move_index(&e, off(&e, SPEC_INDEX, k), index_starts[k], index_lengths[k]);
    for (int64_t k = 0; k < e.keys; k++) {
      set_off(&e, SPEC_INDEX, k, index_starts[k]);
      set_off(&e, SPEC_INDEX_ENDS, k, index_starts[k] + index_lengths[k]);
      slots_from(&e, index_starts[k], index_starts[k] + index_lengths[k]);
    }
    set_off(&e, SPEC_INDEX, e.keys, index_starts[e.keys]);
    free(index_starts);
    free(index_lengths);
  }
  free(starts);
  free(lengths);
  *(int64_t *)(arena + spec[SPEC_MENDS]) = 0;
}

// Where in its target's run of the index the edge at `p` of the table is:
// by its slot, or (without positions) the first of those with its source,
// in the order of the sources' keys, and with its copied fields (edges
// alike in those are alike to a loop over them). -1 where it is not.
static int64_t index_place(const Edges *e, int64_t p) {
  if (e->positions)
    return off(e, SPEC_SLOT, p);
  const int64_t *spec = e->spec;
  int64_t t = target_key(e, p), source = id_at(e, SPEC_SOURCE, p);
  int64_t key = key_of_id(e, source);
  int64_t lo = off(e, SPEC_INDEX, t), hi = off(e, SPEC_INDEX_ENDS, t);
  while (lo < hi) {
    int64_t mid = lo + (hi - lo) / 2;
    if (key_of_id(e, id_at(e, SPEC_SOURCE_COPY, mid)) < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (int64_t j = lo; j < off(e, SPEC_INDEX_ENDS, t) &&
                       key_of_id(e, id_at(e, SPEC_SOURCE_COPY, j)) == key;
       j++) {
    if (id_at(e, SPEC_SOURCE_COPY, j) != source)
      continue;
    int same = 1;
    for (int64_t f = 0; same && f < spec[SPEC_FIELDS]; f++) {
      int64_t offset = spec[SPEC_FIELD0 + 3 * f],
              bytes = spec[SPEC_FIELD0 + 3 * f + 1],
              copy = spec[SPEC_FIELD0 + 3 * f + 2];
      same = !copy || !memcmp(e->arena + copy + j * bytes,
                              e->arena + offset + p * bytes, bytes);
    }
    if (same)
      return j;
  }
  return -1;
}

// The index's entry `j` of the edge at `p` taken out of its target's run.
static void unindex(const Edges *e, int64_t p, int64_t j) {
  int64_t t = target_key(e, p);
  int64_t end = off(e, SPEC_INDEX_ENDS, t);
  move_index(e, j + 1, j, end - j - 1);
  slots_from(e, j, end - 1);
  set_off(e, SPEC_INDEX_ENDS, t, end - 1);
}

// Room for one more edge in key k's run, where it has none, from the
// nearest run that has some: the runs between moved over by one, toward
// it (the edges' places in the index too, with positions). False where no
// run has room, or moving would make `moved` more than `budget`.
static int room_in_table(const Edges *e, int64_t k, int64_t *moved,
                         int64_t budget) {
  char *dead = at(e, SPEC_DEAD);
  int64_t right = k + 1, left = k - 1;
  for (;;) {
    int found = 0;
    if (right < e->keys) {
      int64_t from = off(e, SPEC_SORTED, k + 1), end = off(e, SPEC_ENDS, right);
      if (*moved + (end - from) > budget)
        right = e->keys;
      else if (end < off(e, SPEC_SORTED, right + 1)) {
        move_edges(e, from, from + 1, end - from);
        dead[from] = 2;
        for (int64_t m = k + 1; m <= right; m++) {
          set_off(e, SPEC_SORTED, m, off(e, SPEC_SORTED, m) + 1);
          set_off(e, SPEC_ENDS, m, off(e, SPEC_ENDS, m) + 1);
        }
        if (e->positions)
          for (int64_t q = from + 1; q <= end; q++)
            set_off(e, SPEC_INDEX_EDGES, off(e, SPEC_SLOT, q), q);
        *moved += end - from;
        return 1;
      } else {
        right++;
        found = 1;
      }
    }
    if (left >= 0) {
      int64_t hole = off(e, SPEC_ENDS, left), end = off(e, SPEC_ENDS, k);
      if (*moved + (end - hole) > budget)
        left = -1;
      else if (hole < off(e, SPEC_SORTED, left + 1)) {
        int64_t from = off(e, SPEC_SORTED, left + 1);
        move_edges(e, from, from - 1, end - from);
        dead[end - 1] = 2;
        for (int64_t m = left + 1; m <= k; m++) {
          set_off(e, SPEC_SORTED, m, off(e, SPEC_SORTED, m) - 1);
          set_off(e, SPEC_ENDS, m, off(e, SPEC_ENDS, m) - 1);
        }
        if (e->positions)
          for (int64_t q = from - 1; q < end - 1; q++)
            set_off(e, SPEC_INDEX_EDGES, off(e, SPEC_SLOT, q), q);
        *moved += end - from;
        return 1;
      } else {
        left--;
        found = 1;
      }
    }
    if (!found)
      return 0;
  }
}

// The same for target t's run of the index.
static int room_in_index(const Edges *e, int64_t t, int64_t *moved,
                         int64_t budget) {
  int64_t right = t + 1, left = t - 1;
  for (;;) {
    int found = 0;
    if (right < e->keys) {
      int64_t from = off(e, SPEC_INDEX, t + 1),
              end = off(e, SPEC_INDEX_ENDS, right);
      if (*moved + (end - from) > budget)
        right = e->keys;
      else if (end < off(e, SPEC_INDEX, right + 1)) {
        move_index(e, from, from + 1, end - from);
        for (int64_t m = t + 1; m <= right; m++) {
          set_off(e, SPEC_INDEX, m, off(e, SPEC_INDEX, m) + 1);
          set_off(e, SPEC_INDEX_ENDS, m, off(e, SPEC_INDEX_ENDS, m) + 1);
        }
        slots_from(e, from + 1, end + 1);
        *moved += end - from;
        return 1;
      } else {
        right++;
        found = 1;
      }
    }
    if (left >= 0) {
      int64_t hole = off(e, SPEC_INDEX_ENDS, left),
              end = off(e, SPEC_INDEX_ENDS, t);
      if (*moved + (end - hole) > budget)
        left = -1;
      else if (hole < off(e, SPEC_INDEX, left + 1)) {
        int64_t from = off(e, SPEC_INDEX, left + 1);
        move_index(e, from, from - 1, end - from);
        for (int64_t m = left + 1; m <= t; m++) {
          set_off(e, SPEC_INDEX, m, off(e, SPEC_INDEX, m) - 1);
          set_off(e, SPEC_INDEX_ENDS, m, off(e, SPEC_INDEX_ENDS, m) - 1);
        }
        slots_from(e, from - 1, end - 1);
        *moved += end - from;
        return 1;
      } else {
        left--;
        found = 1;
      }
    }
    if (!found)
      return 0;
  }
}

int32_t ent_edges_mend(char *arena, const int64_t *spec) {
  // (ENT_EDGES_ANEW=1: always sorted anew, to hold mending against.)
  static int anew = -1;
  if (anew < 0) {
    const char *said = getenv("ENT_EDGES_ANEW");
    anew = said && said[0] == '1';
  }
  int64_t *mends = (int64_t *)(arena + spec[SPEC_MENDS]);
  if (mends[1] == WHY_CROWDED) {
    mends[1] = WHY_ROOM;
    return 1;
  }
  mends[1] = WHY_MANY;
  if (anew)
    return 1;
  Edges e = edges_of(arena, spec);
  int64_t *count = (int64_t *)(arena + spec[SPEC_COUNT]);
  int64_t used = off(&e, SPEC_SORTED, e.keys);
  int64_t limit = used / 16;
  if (*count < used || mends[0] + (*count - used) > limit)
    return 1;
  int64_t changed = 0;
  // (What it moves, in all: no more than a quarter of the table, beyond
  // which a sort anew costs less; or than the table, where the moves are
  // of runs side by side and not of slots all over.)
  int64_t moved = 0, budget = e.positions ? used / 4 : used, borrowed = 0;
  // The edges disconnected out of their runs, those after them down by one.
  char *dead = at(&e, SPEC_DEAD);
  for (int64_t p = 0; p < used;) {
    char *found = memchr(dead + p, 1, used - p);
    if (!found)
      break;
    p = found - dead;
    int64_t k = run_key(&e, p), end = off(&e, SPEC_ENDS, k), j = -1;
    moved += end - p;
    if (e.index) {
      j = index_place(&e, p);
      if (j < 0) {
        mends[1] = WHY_ROOM;
        return 1;
      }
      moved += off(&e, SPEC_INDEX_ENDS, target_key(&e, p)) - j;
    }
    if (moved > budget)
      return 1;
    if (e.index)
      unindex(&e, p, j);
    move_edges(&e, p + 1, p, end - p - 1);
    if (e.positions)
      for (int64_t q = p; q < end - 1; q++)
        set_off(&e, SPEC_INDEX_EDGES, off(&e, SPEC_SLOT, q), q);
    dead[end - 1] = 2;
    set_off(&e, SPEC_ENDS, k, end - 1);
    if (++changed + mends[0] > limit)
      return 1;
  }
  // The edges connected since, to the ends of their runs.
  for (int64_t q = used; q < *count; q++) {
    if (dead[q])
      continue;
    int64_t k = run_key(&e, q);
    if (off(&e, SPEC_ENDS, k) >= off(&e, SPEC_SORTED, k + 1)) {
      int64_t before = moved;
      if (!room_in_table(&e, k, &moved, budget)) {
        mends[1] = WHY_ROOM;
        return 1;
      }
      borrowed += moved - before;
    }
    int64_t end = off(&e, SPEC_ENDS, k);
    move_edges(&e, q, end, 1);
    dead[q] = 2;
    set_off(&e, SPEC_ENDS, k, end + 1);
    if (e.index) {
      int64_t t = target_key(&e, end);
      if (off(&e, SPEC_INDEX_ENDS, t) >= off(&e, SPEC_INDEX, t + 1)) {
        int64_t before = moved;
        if (!room_in_index(&e, t, &moved, budget)) {
          mends[1] = WHY_ROOM;
          return 1;
        }
        borrowed += moved - before;
      }
      int64_t first = off(&e, SPEC_INDEX, t),
              last = off(&e, SPEC_INDEX_ENDS, t);
      // (Its place among the target's edges in table order: after those
      // of sources before its own or of its own, which were connected
      // before it.)
      int64_t key = run_key(&e, end), lo = first, hi = last;
      while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (key_of_id(&e, id_at(&e, SPEC_SOURCE_COPY, mid)) <= key)
          lo = mid + 1;
        else
          hi = mid;
      }
      moved += last - lo;
      if (moved > budget)
        return 1;
      move_index(&e, lo, lo + 1, last - lo);
      copy_into_index(&e, end, lo);
      slots_from(&e, lo, last + 1);
      set_off(&e, SPEC_INDEX_ENDS, t, last + 1);
    }
    changed++;
  }
  *count = used;
  mends[0] += changed;
  mends[1] = borrowed > used / 32 ? WHY_CROWDED : WHY_NONE;
  return 0;
}

// The table full while edges are connected: the room taken out of the
// runs, each right after the one before, and the edges connected since
// right after the last; the index's positions moved with them. (Its next
// mending finds no room and sorts anew, which gives room again.)
void ent_edges_squeeze(char *arena, const int64_t *spec) {
  Edges e = edges_of(arena, spec);
  int64_t *count = (int64_t *)(arena + spec[SPEC_COUNT]);
  int64_t used = off(&e, SPEC_SORTED, e.keys), to = 0;
  for (int64_t k = 0; k < e.keys; k++) {
    int64_t from = off(&e, SPEC_SORTED, k), n = off(&e, SPEC_ENDS, k) - from;
    move_edges(&e, from, to, n);
    set_off(&e, SPEC_SORTED, k, to);
    set_off(&e, SPEC_ENDS, k, to + n);
    if (e.positions)
      for (int64_t p = to; p < to + n; p++)
        set_off(&e, SPEC_INDEX_EDGES, off(&e, SPEC_SLOT, p), p);
    to += n;
  }
  set_off(&e, SPEC_SORTED, e.keys, to);
  move_edges(&e, used, to, *count - used);
  *count = to + (*count - used);
}
