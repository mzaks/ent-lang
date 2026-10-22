// What a `for` sends from a loop of its body (`Hull(foe).hp -= 1.0` in a
// `for k in 0..n`) and the edges it connects there or in a `for` over
// edges: what the compiler calls for a program that has such a `for`.
//
// On many threads such a `for` visits its entities in slices of rows,
// 256 of them, each on one thread and in order; on one thread all are
// the first slice. What a slice sends is put on the slice's own list, a
// target's id and a value, 64 bits each (an edge: its two ends, then its
// fields' values two by two; sends to several fields of one entity
// together, ent_sends_put_wide: the id and their values, as many words
// as the list's width says); when the `for` ends the lists are read
// from the first slice's to the last's, which is the order of the rows,
// and for one row the order it was sent in, however many threads there
// were. A list is blocks that are taken as they are needed and kept for
// the next time.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENT_SEND_SLICES 256
#define ENT_SEND_BLOCK 1023
#define ENT_SEND_LISTS 65536

typedef struct ent_send_block {
  struct ent_send_block *next;
  uint64_t count;
  uint64_t sent[2 * ENT_SEND_BLOCK];
} ent_send_block;
// (The compiler reads a block where it is, ent_sends_next_block: how many
// at 8 bytes in, from 16 on each one's id and value.)
_Static_assert(offsetof(ent_send_block, count) == 8, "a block's count");
_Static_assert(offsetof(ent_send_block, sent) == 16, "a block's values");

// (A line of the cache each: every thread writes its own. Where a
// reading of the slice alone is, ent_sends_slice_first.)
typedef struct {
  _Alignas(64) ent_send_block *first;
  ent_send_block *last;
  ent_send_block *block;
  uint64_t at;
  int64_t before;
} ent_send_slice;

// (Where the compiler reads a slice's list where it puts on it without a
// call, ent_sends_slice_list: its last block 8 bytes in.)
_Static_assert(offsetof(ent_send_slice, last) == 8, "a slice's last block");

// What combining took, each way (0: one after another, 1: in groups),
// for each value, so far: and from which call on the way that lost is
// tried again.
typedef struct {
  double cost[2];
  int known[2], best;
  int64_t calls, again, wait;
} ent_send_choice;

// The lists of one sending place, and where the reading of them is.
typedef struct {
  ent_send_slice slices[ENT_SEND_SLICES];
  // The words of an entry: 2, or what ent_sends_put_wide said (the same
  // every time for a place).
  int64_t width;
  int64_t slice;
  ent_send_block *block;
  uint64_t at;
  // (How many there are, read a slice at a time.)
  int64_t sliced;
  // How it went combining them each way (ent_sends_choose), for as many
  // as they were (up to 2^k, k the place); and the way being timed now.
  ent_send_choice choices[64];
  int64_t timed_since;
  int timed_way, timed_size;
  int64_t timed_count;
} ent_send_lists;

// (A table of them for each partition a process runs, devices/ranks.c,
// whose ent_partition_of says which partition's the world is, where each
// call says it is (`world`): the one of any thread of a parallel loop
// that runs for it too; 0 otherwise.)
#define ENT_PARTITIONS 1024
__attribute__((weak)) int ent_partition_of(int64_t world) {
  (void)world;
  return 0;
}
static ent_send_lists **ent_sends_by[ENT_PARTITIONS];
static ent_send_lists **ent_sends_table(int64_t world) {
  int p = ent_partition_of(world);
  ent_send_lists **table = __atomic_load_n(&ent_sends_by[p], __ATOMIC_ACQUIRE);
  if (table)
    return table;
  ent_send_lists **made = calloc(ENT_SEND_LISTS, sizeof(ent_send_lists *));
  if (__atomic_compare_exchange_n(&ent_sends_by[p], &table, made, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    return made;
  free(made);
  return table;
}
#define ent_sends (ent_sends_table(world))

static void *ent_sends_take(size_t size) {
  void *memory = aligned_alloc(64, (size + 63) & ~(size_t)63);
  if (!memory) {
    fprintf(stderr, "ent: no memory for what a 'for' sends from a loop\n");
    abort();
  }
  return memory;
}

// The lists with this number, made when they are first sent to: by
// whichever thread comes first.
static ent_send_lists *ent_sends_of(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (lists)
    return lists;
  if (number >= ENT_SEND_LISTS) {
    fprintf(stderr, "ent: too many places that send from a loop\n");
    abort();
  }
  ent_send_lists *made = ent_sends_take(sizeof(ent_send_lists));
  for (int slice = 0; slice < ENT_SEND_SLICES; ++slice)
    made->slices[slice].first = made->slices[slice].last = 0;
  made->slice = ENT_SEND_SLICES;
  made->block = 0;
  made->at = 0;
  made->width = 2;
  made->sliced = 0;
  memset(made->choices, 0, sizeof made->choices);
  made->timed_way = -1;
  if (__atomic_compare_exchange_n(&ent_sends[number], &lists, made, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    return made;
  free(made);
  return lists;
}

void ent_sends_put(int64_t world, int64_t number, int64_t slice, int64_t id,
                   int64_t value) {
  ent_send_slice *list = &ent_sends_of(world, number)->slices[slice];
  ent_send_block *block = list->last;
  if (!block || block->count == ENT_SEND_BLOCK) {
    // (The next of those kept from the last time, if there is one.)
    ent_send_block *next = block ? block->next : list->first;
    if (!next) {
      next = ent_sends_take(sizeof(ent_send_block));
      next->next = 0;
      if (block)
        block->next = next;
      else
        list->first = next;
    }
    next->count = 0;
    list->last = block = next;
  }
  block->sent[2 * block->count] = (uint64_t)id;
  block->sent[2 * block->count + 1] = (uint64_t)value;
  block->count += 1;
}

// A slice's list, taken where it has no block or its last is full: the
// next block, empty (one kept from the last time, or a new one).
static ent_send_block *ent_sends_grow(ent_send_slice *list) {
  ent_send_block *block = list->last;
  ent_send_block *next = block ? block->next : list->first;
  if (!next) {
    next = ent_sends_take(sizeof(ent_send_block));
    next->next = 0;
    if (block)
      block->next = next;
    else
      list->first = next;
  }
  next->count = 0;
  list->last = next;
  return next;
}

// What the compiler puts on without a call: where a slice's list is (its
// `last` block 8 bytes in, a block's count 8 bytes in and its entries from
// 16 on, `width` words each, 2 * ENT_SEND_BLOCK / width of them); and the
// next block, where the last is full or there is none.
int64_t ent_sends_slice_list(int64_t world, int64_t number, int64_t slice,
                             int64_t width) {
  ent_send_lists *lists = ent_sends_of(world, number);
  lists->width = width;
  return (int64_t)(uintptr_t)&lists->slices[slice];
}
int64_t ent_sends_slice_grow(int64_t world, int64_t list) {
  (void)world;
  return (int64_t)(uintptr_t)ent_sends_grow((ent_send_slice *)(uintptr_t)list);
}

// Several values sent to one entity together: its id and `width` - 1
// values (up to 7; those after are nothing), as one entry.
void ent_sends_put_wide(int64_t world, int64_t number, int64_t slice,
                        int64_t width, int64_t id, int64_t v0, int64_t v1,
                        int64_t v2, int64_t v3, int64_t v4, int64_t v5,
                        int64_t v6) {
  ent_send_lists *lists = ent_sends_of(world, number);
  lists->width = width;
  ent_send_slice *list = &lists->slices[slice];
  ent_send_block *block = list->last;
  uint64_t fits = 2 * ENT_SEND_BLOCK / (uint64_t)width;
  if (!block || block->count == fits)
    block = ent_sends_grow(list);
  uint64_t *entry = &block->sent[(uint64_t)width * block->count];
  // (Each word written out: a copy of so few is a call otherwise.)
  switch (width) {
  case 8: entry[7] = (uint64_t)v6; /* fall through */
  case 7: entry[6] = (uint64_t)v5; /* fall through */
  case 6: entry[5] = (uint64_t)v4; /* fall through */
  case 5: entry[4] = (uint64_t)v3; /* fall through */
  case 4: entry[3] = (uint64_t)v2; /* fall through */
  case 3: entry[2] = (uint64_t)v1; /* fall through */
  default: entry[1] = (uint64_t)v0;
  }
  entry[0] = (uint64_t)id;
  block->count += 1;
}

// How many were sent, and the reading starts at the first.
int64_t ent_sends_count(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (!lists)
    return 0;
  int64_t count = 0;
  for (int slice = 0; slice < ENT_SEND_SLICES; ++slice) {
    ent_send_slice *list = &lists->slices[slice];
    for (ent_send_block *block = list->first; block; block = block->next) {
      count += block->count;
      if (block == list->last)
        break;
    }
  }
  lists->slice = -1;
  lists->block = 0;
  lists->at = 0;
  return count;
}

// The one the reading is at: the next list's first where a list is done.
static ent_send_block *ent_sends_at(ent_send_lists *lists) {
  while (!lists->block || lists->at == lists->block->count) {
    ent_send_block *block = lists->block;
    if (block && block != lists->slices[lists->slice].last) {
      lists->block = block->next;
    } else {
      lists->slice += 1;
      lists->block = lists->slices[lists->slice].last
                         ? lists->slices[lists->slice].first
                         : 0;
    }
    lists->at = 0;
  }
  return lists->block;
}

// Or the next block that has any, by its address (0: none left), for the
// compiler to read them from (two words each, see ent_send_block).
int64_t ent_sends_next_block(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (!lists)
    return 0;
  for (;;) {
    ent_send_block *block = lists->block;
    if (block && block != lists->slices[lists->slice].last) {
      block = block->next;
    } else {
      do
        lists->slice += 1;
      while (lists->slice < ENT_SEND_SLICES &&
             !lists->slices[lists->slice].last);
      if (lists->slice >= ENT_SEND_SLICES)
        return 0;
      block = lists->slices[lists->slice].first;
    }
    lists->block = block;
    if (block->count)
      return (int64_t)(uintptr_t)block;
  }
}

// Whom the next one was sent to; and what, which goes on to the one after.
int64_t ent_sends_id(int64_t world, int64_t number) {
  ent_send_lists *lists = ent_sends[number];
  return (int64_t)ent_sends_at(lists)->sent[2 * lists->at];
}
int64_t ent_sends_value(int64_t world, int64_t number) {
  ent_send_lists *lists = ent_sends[number];
  ent_send_block *block = ent_sends_at(lists);
  return (int64_t)block->sent[2 * lists->at++ + 1];
}

// Or read a slice at a time, each slice on a thread of its own: how many
// were sent, and how many in the slices before each (ent_sends_slices);
// how many before `slice` (ent_sends_slice_before, ENT_SEND_SLICES: all
// of them); and its own from the first (ent_sends_slice_first, then
// ent_sends_slice_id and ent_sends_slice_value as above).
int64_t ent_sends_slices(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (!lists)
    return 0;
  int64_t count = 0;
  for (int slice = 0; slice < ENT_SEND_SLICES; ++slice) {
    ent_send_slice *list = &lists->slices[slice];
    list->before = count;
    for (ent_send_block *block = list->first; block; block = block->next) {
      count += block->count;
      if (block == list->last)
        break;
    }
  }
  lists->sliced = count;
  return count;
}
int64_t ent_sends_slice_before(int64_t world, int64_t number, int64_t slice) {
  ent_send_lists *lists = ent_sends[number];
  return slice == ENT_SEND_SLICES ? lists->sliced : lists->slices[slice].before;
}
void ent_sends_slice_first(int64_t world, int64_t number, int64_t slice) {
  ent_send_slice *list = &ent_sends[number]->slices[slice];
  list->block = list->last ? list->first : 0;
  list->at = 0;
}
static ent_send_block *ent_sends_slice_at(ent_send_slice *list) {
  if (list->at == list->block->count) {
    list->block = list->block->next;
    list->at = 0;
  }
  return list->block;
}
int64_t ent_sends_slice_id(int64_t world, int64_t number, int64_t slice) {
  ent_send_slice *list = &ent_sends[number]->slices[slice];
  return (int64_t)ent_sends_slice_at(list)->sent[2 * list->at];
}
int64_t ent_sends_slice_value(int64_t world, int64_t number, int64_t slice) {
  ent_send_slice *list = &ent_sends[number]->slices[slice];
  return (int64_t)ent_sends_slice_at(list)->sent[2 * list->at++ + 1];
}

// Or, of what is sent to entities on many threads, as many lists as the
// compiler makes groups of the targets (each a range of their slots), a
// slice's values for each group on that group's list: each list is read
// as above, the groups' at once, a group on a thread, which combines each
// target's values in the order of one list (LowerToLoops, kSendGroups).

// Which way to combine the `count` values sent (1: in groups, on many
// threads; 0: one after another), each giving the same: the one that has
// taken less for each value, for about as many values (from `least` on;
// fewer, one after another). The first time `guess` (in groups from so
// many), the second the other way; the way that lost is tried again
// after 8 more, then twice as many each time it loses again (up to 1024),
// in case it no longer does. It is timed until ent_sends_chosen.
// ENT_COMBINE, where the program runs, says `groups` or `one` instead.
static int ent_sends_forced = -2;
static int64_t ent_sends_clock(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int64_t ent_sends_choose(int64_t world, int64_t number, int64_t count,
                         int64_t least, int64_t guess) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (ent_sends_forced == -2) {
    const char *asked = getenv("ENT_COMBINE");
    ent_sends_forced = !asked                         ? -1
                       : strcmp(asked, "groups") == 0 ? 1
                       : strcmp(asked, "one") == 0    ? 0
                                                      : -1;
  }
  if (!lists || count <= 0)
    return 0;
  lists->timed_way = -1;
  if (ent_sends_forced >= 0)
    return ent_sends_forced;
  if (count < least)
    return 0;
  int size = 63 - __builtin_clzll((uint64_t)count);
  ent_send_choice *choice = &lists->choices[size];
  int first = count >= guess;
  int best = choice->known[0] && choice->known[1] ? choice->best
             : choice->known[0]                   ? 0
             : choice->known[1]                   ? 1
                                                  : first;
  choice->calls += 1;
  int way = best;
  if (choice->calls == 2 && !choice->known[!best])
    way = !best;
  else if (choice->again && choice->calls >= choice->again)
    way = !best;
  lists->timed_way = way;
  lists->timed_size = size;
  lists->timed_count = count;
  lists->timed_since = ent_sends_clock();
  return way;
}
void ent_sends_chosen(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (!lists || lists->timed_way < 0)
    return;
  double each =
      (double)(ent_sends_clock() - lists->timed_since) / lists->timed_count;
  ent_send_choice *choice = &lists->choices[lists->timed_size];
  int way = lists->timed_way;
  int both = choice->known[0] && choice->known[1];
  int was = both ? choice->best : way;
  // (A time can only be longer than what it takes, the machine doing
  // something else meanwhile: one below what is known counts for half,
  // one above for a tenth.)
  double known = choice->cost[way];
  choice->cost[way] = !choice->known[way] ? each
                      : each < known      ? 0.5 * known + 0.5 * each
                                          : 0.9 * known + 0.1 * each;
  choice->known[way] = 1;
  lists->timed_way = -1;
  if (!choice->known[0] || !choice->known[1])
    return;
  // (The other way only where it takes 5% less: not back and forth. The
  // first time both are known, the one that took less.)
  int best = was;
  if (!both)
    best = choice->cost[1] < choice->cost[0];
  else if (choice->cost[!was] < 0.95 * choice->cost[was])
    best = !was;
  choice->best = best;
  if (best != was || !choice->wait)
    choice->wait = 8;
  else if (way != best)
    choice->wait = choice->wait < 1024 ? 2 * choice->wait : 1024;
  if (way != best || best != was || !choice->again)
    choice->again = choice->calls + choice->wait;
}

// Nothing is on the lists any more; their blocks are kept.
void ent_sends_clear(int64_t world, int64_t number) {
  ent_send_lists *lists = __atomic_load_n(&ent_sends[number], __ATOMIC_ACQUIRE);
  if (!lists)
    return;
  for (int slice = 0; slice < ENT_SEND_SLICES; ++slice) {
    ent_send_slice *list = &lists->slices[slice];
    for (ent_send_block *block = list->first; block; block = block->next)
      block->count = 0;
    list->last = 0;
  }
}

// The rows a reactive `for` visits where what it sends is combined as it
// goes and the order shows (fractions, which come out other where added
// in another order): gathered from its logs, one thread, then put in the
// order of the rows, which is the order a `for` through all of them has.
// A list for each such `for` and archetype; its room is kept.
typedef struct {
  int64_t *rows;
  int64_t count, room;
} ent_rows_list;

static ent_rows_list *ent_rows_by[ENT_PARTITIONS];
static ent_rows_list *ent_rows_table(int64_t world) {
  int p = ent_partition_of(world);
  ent_rows_list *table = __atomic_load_n(&ent_rows_by[p], __ATOMIC_ACQUIRE);
  if (table)
    return table;
  ent_rows_list *made = calloc(ENT_SEND_LISTS, sizeof(ent_rows_list));
  if (__atomic_compare_exchange_n(&ent_rows_by[p], &table, made, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    return made;
  free(made);
  return table;
}
#define ent_rows (ent_rows_table(world))

void ent_rows_put(int64_t world, int64_t number, int64_t row) {
  ent_rows_list *list = &ent_rows[number];
  if (list->count == list->room) {
    int64_t room = list->room ? 2 * list->room : 1024;
    int64_t *rows = realloc(list->rows, (size_t)room * sizeof(int64_t));
    if (!rows) {
      fprintf(stderr, "ent: no memory for the rows a 'for' visits\n");
      abort();
    }
    list->rows = rows;
    list->room = room;
  }
  list->rows[list->count++] = row;
}

static int ent_rows_compare(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return (x > y) - (x < y);
}

// How many rows were put, now in order; the list is empty for the next
// time, and its rows can be read until then.
int64_t ent_rows_sort(int64_t world, int64_t number) {
  ent_rows_list *list = &ent_rows[number];
  int64_t count = list->count;
  qsort(list->rows, (size_t)count, sizeof(int64_t), ent_rows_compare);
  list->count = 0;
  return count;
}

int64_t ent_rows_at(int64_t world, int64_t number, int64_t k) {
  return ent_rows[number].rows[k];
}
