// Texts of any length (`text` without a capacity; `!ent.string`): what
// the compiler calls for a program that has one (devices/text.ent).
//
// A text is a block: its length, 32 bits, and its bytes after that. A
// value in the program is the address of one (0 for a text without
// bytes); a field holds a block of its own, which `ent_text_own` makes
// and `ent_text_drop` gives back.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t length;
  char bytes[];
} ent_text;

// The text without bytes: a length of 0, and the 0 after its bytes.
static const struct {
  uint32_t length;
  char bytes[4];
} ent_text_nothing = {0, {0}};
#define ent_text_none (*(const ent_text *)&ent_text_nothing)

// How many blocks fields hold, and how many joining made that are still
// kept; a program run with ENT_TEXT_REPORT set says so at its end (for
// tests of what is given back).
#include <stdio.h>
static long ent_text_held, ent_text_kept;
static void ent_text_report(void) {
  fprintf(stderr, "texts held: %ld\n", ent_text_held);
  // (Those made by joining, and not given back when their schedule was
  // done: none.)
  fprintf(stderr, "texts kept: %ld\n", ent_text_kept);
}
static void ent_text_count(long by) {
  static int asked;
  if (!__atomic_exchange_n(&asked, 1, __ATOMIC_RELAXED) &&
      getenv("ENT_TEXT_REPORT"))
    atexit(ent_text_report);
  __atomic_add_fetch(&ent_text_held, by, __ATOMIC_RELAXED);
}

static const ent_text *ent_text_of(uint64_t view) {
  return view ? (const ent_text *)(uintptr_t)view : &ent_text_none;
}

// The block as C gets it: never no address.
uint64_t ent_text_view(uint64_t view) {
  return (uint64_t)(uintptr_t)ent_text_of(view);
}

// A block of its own with the bytes of `view` (and a 0 after them, which
// is not counted); none for no bytes.
uint64_t ent_text_own(uint64_t view) {
  const ent_text *from = ent_text_of(view);
  if (!from->length)
    return 0;
  ent_text *held = malloc(sizeof(ent_text) + from->length + 1);
  if (!held)
    abort();
  held->length = from->length;
  memcpy(held->bytes, from->bytes, from->length);
  held->bytes[from->length] = 0;
  ent_text_count(1);
  return (uint64_t)(uintptr_t)held;
}

// The blocks that are nobody's: made by joining, kept until the schedule
// that runs is done. (Any thread may make one; they are given back where
// only one runs.)
typedef struct ent_text_made {
  struct ent_text_made *next;
  // (An `ent_text` from here on.)
  uint32_t length;
  char bytes[];
} ent_text_made;
static ent_text_made *ent_text_all_made;

static ent_text *ent_text_make(uint32_t length) {
  ent_text_made *made = malloc(sizeof(ent_text_made) + length + 1);
  if (!made)
    abort();
  __atomic_add_fetch(&ent_text_kept, 1, __ATOMIC_RELAXED);
  made->length = length;
  made->bytes[length] = 0;
  made->next = __atomic_load_n(&ent_text_all_made, __ATOMIC_RELAXED);
  while (!__atomic_compare_exchange_n(&ent_text_all_made, &made->next, made,
                                      1, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    ;
  return (ent_text *)&made->length;
}

// The bytes of `a` and then those of `b`.
uint64_t ent_text_join(uint64_t a, uint64_t b) {
  const ent_text *left = ent_text_of(a), *right = ent_text_of(b);
  if (!left->length && !right->length)
    return 0;
  ent_text *both = ent_text_make(left->length + right->length);
  memcpy(both->bytes, left->bytes, left->length);
  memcpy(both->bytes + left->length, right->bytes, right->length);
  return (uint64_t)(uintptr_t)both;
}

// The same as a block of a field's own: for a field that is given what
// is joined, without a copy in between.
uint64_t ent_text_join_own(uint64_t a, uint64_t b) {
  const ent_text *left = ent_text_of(a), *right = ent_text_of(b);
  uint32_t length = left->length + right->length;
  if (!length)
    return 0;
  ent_text *held = malloc(sizeof(ent_text) + length + 1);
  if (!held)
    abort();
  held->length = length;
  memcpy(held->bytes, left->bytes, left->length);
  memcpy(held->bytes + left->length, right->bytes, right->length);
  held->bytes[length] = 0;
  ent_text_count(1);
  return (uint64_t)(uintptr_t)held;
}

// A copy that is kept as long as a joined text is.
uint64_t ent_text_keep(uint64_t view) {
  const ent_text *from = ent_text_of(view);
  if (!from->length)
    return 0;
  ent_text *kept = ent_text_make(from->length);
  memcpy(kept->bytes, from->bytes, from->length);
  return (uint64_t)(uintptr_t)kept;
}

void ent_text_release(void) {
  ent_text_made *made =
      __atomic_exchange_n(&ent_text_all_made, 0, __ATOMIC_ACQUIRE);
  while (made) {
    ent_text_made *next = made->next;
    free(made);
    __atomic_sub_fetch(&ent_text_kept, 1, __ATOMIC_RELAXED);
    made = next;
  }
}

void ent_text_drop(uint64_t held) {
  if (held)
    ent_text_count(-1);
  free((void *)(uintptr_t)held);
}

int32_t ent_text_length(uint64_t view) {
  return (int32_t)ent_text_of(view)->length;
}

// Byte `index`; 0 where there is none.
int8_t ent_text_at(uint64_t view, int64_t index) {
  const ent_text *text = ent_text_of(view);
  return index >= 0 && index < (int64_t)text->length
             ? (int8_t)text->bytes[index]
             : 0;
}

int8_t ent_text_equal(uint64_t a, uint64_t b) {
  const ent_text *left = ent_text_of(a), *right = ent_text_of(b);
  return left->length == right->length &&
         !memcmp(left->bytes, right->bytes, left->length);
}

// Into a text of a capacity (`out`: its 16-bit length, then its bytes,
// all zero so far): as many bytes as it holds.
void ent_text_cut(uint64_t view, uint64_t out, int32_t capacity) {
  const ent_text *text = ent_text_of(view);
  uint16_t length =
      (uint16_t)(text->length < (uint32_t)capacity ? text->length
                                                   : (uint32_t)capacity);
  char *to = (char *)(uintptr_t)out;
  memcpy(to, &length, sizeof length);
  memcpy(to + sizeof length, text->bytes, length);
}
