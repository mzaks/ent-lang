// What a `for` sends from a loop of its body (`Hull(foe).hp -= 1.0` in a
// `for k in 0..n`) and the edges it connects there or in a `for` over
// edges: what the compiler calls for a program that has such a `for`.
//
// On many threads such a `for` visits its entities in slices of rows,
// 256 of them, each on one thread and in order; on one thread all are
// the first slice. What a slice sends is put on the slice's own list, a
// target's id and a value, 64 bits each (an edge: its two ends, then its
// fields' values two by two); when the `for` ends the lists are read
// from the first slice's to the last's, which is the order of the rows,
// and for one row the order it was sent in, however many threads there
// were. A list is blocks that are taken as they are needed and kept for
// the next time.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ENT_SEND_SLICES 256
#define ENT_SEND_BLOCK 1023
#define ENT_SEND_LISTS 65536

typedef struct ent_send_block {
  struct ent_send_block *next;
  uint64_t count;
  uint64_t sent[2 * ENT_SEND_BLOCK];
} ent_send_block;

// (A line of the cache each: every thread writes its own.)
typedef struct {
  _Alignas(64) ent_send_block *first;
  ent_send_block *last;
} ent_send_slice;

// The lists of one sending place, and where the reading of them is.
typedef struct {
  ent_send_slice slices[ENT_SEND_SLICES];
  int64_t slice;
  ent_send_block *block;
  uint64_t at;
} ent_send_lists;

static ent_send_lists *ent_sends[ENT_SEND_LISTS];

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
static ent_send_lists *ent_sends_of(int64_t number) {
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
  if (__atomic_compare_exchange_n(&ent_sends[number], &lists, made, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    return made;
  free(made);
  return lists;
}

void ent_sends_put(int64_t number, int64_t slice, int64_t id, int64_t value) {
  ent_send_slice *list = &ent_sends_of(number)->slices[slice];
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

// How many were sent, and the reading starts at the first.
int64_t ent_sends_count(int64_t number) {
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

// Whom the next one was sent to; and what, which goes on to the one after.
int64_t ent_sends_id(int64_t number) {
  ent_send_lists *lists = ent_sends[number];
  return (int64_t)ent_sends_at(lists)->sent[2 * lists->at];
}
int64_t ent_sends_value(int64_t number) {
  ent_send_lists *lists = ent_sends[number];
  ent_send_block *block = ent_sends_at(lists);
  return (int64_t)block->sent[2 * lists->at++ + 1];
}

// Nothing is on the lists any more; their blocks are kept.
void ent_sends_clear(int64_t number) {
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

static ent_rows_list ent_rows[ENT_SEND_LISTS];

void ent_rows_put(int64_t number, int64_t row) {
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
int64_t ent_rows_sort(int64_t number) {
  ent_rows_list *list = &ent_rows[number];
  int64_t count = list->count;
  qsort(list->rows, (size_t)count, sizeof(int64_t), ent_rows_compare);
  list->count = 0;
  return count;
}

int64_t ent_rows_at(int64_t number, int64_t k) { return ent_rows[number].rows[k]; }
