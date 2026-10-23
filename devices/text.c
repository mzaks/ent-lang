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
// (A list for each partition a process runs, devices/ranks.c, whose
// ent_partition_index says which is the calling one's; 0 otherwise: each
// gives back its own.)
__attribute__((weak)) int ent_partition_index(void) { return 0; }
__attribute__((weak)) int ent_partitions_here(void) { return 1; }
static ent_text_made *ent_text_all_made_by[1024];
#define ent_text_all_made (ent_text_all_made_by[ent_partition_index()])

// (Where a process runs several partitions, a thread of a parallel loop
// is none of theirs, and the partition it runs for unknown: what it makes
// is on a list of the process's, one for each round, a round ending when
// every partition has given back once. What is made in one round is
// given back when the next ends: every partition has given back since,
// which none does while it still uses what was made for it.)
__attribute__((weak)) int omp_in_parallel(void);
static ent_text_made *ent_text_made_in[3];
static int64_t ent_text_round = 0;
static char ent_text_gave[1024];
static int ent_text_gave_count = 0;
static char ent_text_rounds_lock = 0;

static void ent_text_push(ent_text_made **list, ent_text_made *made) {
  made->next = __atomic_load_n(list, __ATOMIC_RELAXED);
  while (!__atomic_compare_exchange_n(list, &made->next, made, 1,
                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    ;
}

static ent_text *ent_text_make(uint32_t length) {
  ent_text_made *made = malloc(sizeof(ent_text_made) + length + 1);
  if (!made)
    abort();
  __atomic_add_fetch(&ent_text_kept, 1, __ATOMIC_RELAXED);
  made->length = length;
  made->bytes[length] = 0;
  if (ent_partitions_here() > 1 && omp_in_parallel && omp_in_parallel())
    ent_text_push(
        &ent_text_made_in[__atomic_load_n(&ent_text_round, __ATOMIC_ACQUIRE) %
                          3],
        made);
  else
    ent_text_push(&ent_text_all_made, made);
  return (ent_text *)&made->length;
}

static void ent_text_free(ent_text_made *made) {
  while (made) {
    ent_text_made *next = made->next;
    free(made);
    __atomic_sub_fetch(&ent_text_kept, 1, __ATOMIC_RELAXED);
    made = next;
  }
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
  ent_text_free(__atomic_exchange_n(&ent_text_all_made, 0, __ATOMIC_ACQUIRE));
  int partitions = ent_partitions_here();
  if (partitions <= 1)
    return;
  // (This partition has given back in this round; the last to do so ends
  // it, and what was made in the one before is given back.)
  while (__atomic_test_and_set(&ent_text_rounds_lock, __ATOMIC_ACQUIRE))
    ;
  ent_text_made *done = 0;
  int partition = ent_partition_index();
  if (!ent_text_gave[partition]) {
    ent_text_gave[partition] = 1;
    if (++ent_text_gave_count == partitions) {
      int64_t round = ent_text_round;
      done = __atomic_exchange_n(&ent_text_made_in[(round + 2) % 3], 0,
                                 __ATOMIC_ACQUIRE);
      memset(ent_text_gave, 0, sizeof ent_text_gave);
      ent_text_gave_count = 0;
      __atomic_store_n(&ent_text_round, round + 1, __ATOMIC_RELEASE);
    }
  }
  __atomic_clear(&ent_text_rounds_lock, __ATOMIC_RELEASE);
  ent_text_free(done);
}

// Every partition of the process done: what threads of parallel loops
// made is nobody's any more (devices/ranks.c).
void ent_text_release_all(void) {
  for (int round = 0; round < 3; ++round)
    ent_text_free(
        __atomic_exchange_n(&ent_text_made_in[round], 0, __ATOMIC_ACQUIRE));
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

// A hash of a text (`hash_of`, ent.text.hash): its bytes 16 at a time,
// two little-endian words of 8 (0 past its end), each block mixed into
// the hash from `seed` as rapidhash mixes (one 64 by 64 bit multiply, the
// halves of the product xored), then with its length.
static inline uint64_t ent_text_mix(uint64_t a, uint64_t b) {
  __uint128_t product = (__uint128_t)a * b;
  return (uint64_t)product ^ (uint64_t)(product >> 64);
}
int64_t ent_text_hash(uint64_t view, int64_t seed) {
  const ent_text *text = ent_text_of(view);
  const uint64_t secret0 = 0x2d358dccaa6c78a5ull,
                 secret1 = 0x8bb84b93962eacc9ull,
                 secret2 = 0x4b33a62ed433d4a3ull;
  uint64_t length = text->length, hash = (uint64_t)seed;
  for (uint64_t from = 0; from < length; from += 16) {
    uint64_t words[2] = {0, 0};
    memcpy(words, text->bytes + from, length - from < 16 ? length - from : 16);
    hash = ent_text_mix(words[0] ^ secret1, words[1] ^ hash);
  }
  return (int64_t)ent_text_mix(hash ^ secret0, length ^ secret2);
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
