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
// (StringZilla, where it is found: searches and the bytes made lower case
// by the processor's vector instructions, those the compiler is told of.)
#if defined(__has_include)
#if __has_include("stringzilla/find.h")
#include "stringzilla/find.h"
#include "stringzilla/memory.h"
// (Where it has vector code for the processor: without, the C library's
// memchr, which picks its own as the program runs, is as fast.)
#if SZ_USE_HASWELL || SZ_USE_SKYLAKE || SZ_USE_ICELAKE || SZ_USE_NEON ||      \
    SZ_USE_SVE
#define ENT_TEXT_STRINGZILLA 1
#endif
#endif
#endif

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
static long ent_text_in_arenas(void);
static void ent_text_report(void) {
  fprintf(stderr, "texts held: %ld\n", ent_text_held);
  // (Those made by joining, and not given back when their schedule was
  // done: none.)
  fprintf(stderr, "texts kept: %ld\n", ent_text_kept + ent_text_in_arenas());
}
// (Counted only where the report is asked for: whether it is, is looked
// up once, and after that read, not written, by every thread.)
static int ent_text_reporting; // 0: not looked up yet, 1: no, 2: yes
static int ent_text_reports(void) {
  int state = __atomic_load_n(&ent_text_reporting, __ATOMIC_RELAXED);
  if (state)
    return state == 2;
  int want = getenv("ENT_TEXT_REPORT") ? 2 : 1, none = 0;
  if (__atomic_compare_exchange_n(&ent_text_reporting, &none, want, 0,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
      want == 2)
    atexit(ent_text_report);
  return __atomic_load_n(&ent_text_reporting, __ATOMIC_RELAXED) == 2;
}
static void ent_text_count(long by) {
  if (ent_text_reports())
    __atomic_add_fetch(&ent_text_held, by, __ATOMIC_RELAXED);
}

static const ent_text *ent_text_of(uint64_t view) {
  return view ? (const ent_text *)(uintptr_t)view : &ent_text_none;
}

// The block as C gets it: never no address.
uint64_t ent_text_view(uint64_t view) {
  return (uint64_t)(uintptr_t)ent_text_of(view);
}

// Where the blocks fields hold live. With ENT_WORLD_DIR set (as for the
// world, devices/memory.c) they are in a file in that directory, made
// sparse and gone when the program ends, mapped shared: the system keeps
// in memory the blocks the program goes to now and writes the others to
// the file, so that a world larger than the machine's memory has its
// texts there too. A block is as big as the power of two its text needs
// (16 bytes at least), kept for the next of that size when it is given
// back; the file's room is taken from its start, and is 1 TB, of which
// only what is written takes room on disk (ENT_TEXT_FILE_BYTES says
// another). Without it, the C library's malloc.
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#define ENT_HELD_CLASSES 40
static char *ent_held_file;  // 0: not looked at yet, or malloc
static size_t ent_held_room, ent_held_top;
static int ent_held_state;   // 0: not looked at, 1: malloc, 2: the file
static void *ent_held_free[ENT_HELD_CLASSES];
static char ent_held_locks[ENT_HELD_CLASSES];
static char ent_held_starting;

static int ent_held_in_file(void) {
  int state = __atomic_load_n(&ent_held_state, __ATOMIC_ACQUIRE);
  if (state)
    return state == 2;
  while (__atomic_test_and_set(&ent_held_starting, __ATOMIC_ACQUIRE))
    ;
  state = ent_held_state;
  if (!state) {
    state = 1;
    const char *directory = getenv("ENT_WORLD_DIR");
    if (directory && *directory) {
      const char *bytes = getenv("ENT_TEXT_FILE_BYTES");
      size_t room = bytes && *bytes ? (size_t)strtoull(bytes, 0, 10)
                                    : (size_t)1 << 40;
      char path[4096];
      snprintf(path, sizeof path, "%s/ent-texts.%ld.XXXXXX", directory,
               (long)getpid());
      int file = mkstemp(path);
      void *base = MAP_FAILED;
      if (file >= 0) {
        unlink(path);
        if (ftruncate(file, (off_t)room) == 0)
          base = mmap(0, room, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_NORESERVE, file, 0);
        close(file);
      }
      if (base == MAP_FAILED) {
        fprintf(stderr, "ent: no file for the texts in %s\n", directory);
        exit(1);
      }
      ent_held_file = base;
      ent_held_room = room;
      state = 2;
    }
    __atomic_store_n(&ent_held_state, state, __ATOMIC_RELEASE);
  }
  __atomic_clear(&ent_held_starting, __ATOMIC_RELEASE);
  return state == 2;
}

// The size class of a block of `bytes`: 2^class bytes.
static int ent_held_class(size_t bytes) {
  int c = 4;
  while (((size_t)1 << c) < bytes)
    ++c;
  return c;
}

static void *ent_held_new(size_t bytes) {
  if (!ent_held_in_file())
    return malloc(bytes);
  int c = ent_held_class(bytes);
  while (__atomic_test_and_set(&ent_held_locks[c], __ATOMIC_ACQUIRE))
    ;
  void *block = ent_held_free[c];
  if (block)
    memcpy(&ent_held_free[c], block, sizeof block);
  __atomic_clear(&ent_held_locks[c], __ATOMIC_RELEASE);
  if (block)
    return block;
  size_t at = __atomic_fetch_add(&ent_held_top, (size_t)1 << c,
                                 __ATOMIC_RELAXED);
  if (at + ((size_t)1 << c) > ent_held_room) {
    fprintf(stderr, "ent: the file for the texts is full (%zu bytes; "
                    "ENT_TEXT_FILE_BYTES says more)\n", ent_held_room);
    exit(3);
  }
  return ent_held_file + at;
}

// A block given back: `bytes` is what it was made for.
static void ent_held_give(void *block, size_t bytes) {
  if (!block)
    return;
  if (!ent_held_in_file()) {
    free(block);
    return;
  }
  int c = ent_held_class(bytes);
  while (__atomic_test_and_set(&ent_held_locks[c], __ATOMIC_ACQUIRE))
    ;
  memcpy(block, &ent_held_free[c], sizeof block);
  ent_held_free[c] = block;
  __atomic_clear(&ent_held_locks[c], __ATOMIC_RELEASE);
}
#else
static void *ent_held_new(size_t bytes) { return malloc(bytes); }
static void ent_held_give(void *block, size_t bytes) {
  (void)bytes;
  free(block);
}
#endif

// A block of its own with the bytes of `view` (and a 0 after them, which
// is not counted); none for no bytes.
uint64_t ent_text_own(uint64_t view) {
  const ent_text *from = ent_text_of(view);
  if (!from->length)
    return 0;
  ent_text *held = ent_held_new(sizeof(ent_text) + from->length + 1);
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
// (Weak: a build without OpenMP has none, and it is then null. On macOS
// an undefined weak symbol is weak_import, and the link says it may be
// missing, -Wl,-U, see tools/ent.)
#if defined(__APPLE__)
__attribute__((weak_import)) int omp_in_parallel(void);
#else
__attribute__((weak)) int omp_in_parallel(void);
#endif
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

// Where one partition runs (no ranks, or one), what a thread makes is cut
// from an arena of its own, its blocks one after another in chunks, and
// given back all at once: the arenas start again from their newest
// chunk, the older ones and the blocks too big for a chunk freed.
typedef struct ent_text_chunk {
  struct ent_text_chunk *next;
  size_t room;
  char bytes[];
} ent_text_chunk;
typedef struct ent_text_arena {
  ent_text_chunk *chunk; // (the newest; the older ones after it)
  size_t used;
  ent_text_chunk *big; // (one a block)
  long count;
  struct ent_text_arena *next; // (every thread's)
} ent_text_arena;
static ent_text_arena *ent_text_arenas;
static __thread ent_text_arena *ent_text_mine;
enum { ent_text_first_chunk = 1 << 16, ent_text_most_chunk = 1 << 22 };

static ent_text_chunk *ent_text_chunk_new(size_t room, ent_text_chunk *next) {
  ent_text_chunk *chunk = malloc(sizeof(ent_text_chunk) + room);
  if (!chunk)
    abort();
  chunk->next = next;
  chunk->room = room;
  return chunk;
}

static ent_text *ent_text_cut_out(uint32_t length) {
  ent_text_arena *arena = ent_text_mine;
  if (!arena) {
    arena = calloc(1, sizeof(ent_text_arena));
    if (!arena)
      abort();
    arena->next = __atomic_load_n(&ent_text_arenas, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&ent_text_arenas, &arena->next, arena,
                                        1, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
      ;
    ent_text_mine = arena;
    ent_text_count(0); // (said at the end where asked)
  }
  ++arena->count;
  // (The length, the bytes and a 0, 8-byte aligned.)
  size_t size = (sizeof(uint32_t) + (size_t)length + 1 + 7) & ~(size_t)7;
  if (size > ent_text_most_chunk / 4) {
    arena->big = ent_text_chunk_new(size, arena->big);
    ent_text *text = (ent_text *)arena->big->bytes;
    text->length = length;
    text->bytes[length] = 0;
    return text;
  }
  if (!arena->chunk || arena->used + size > arena->chunk->room) {
    size_t room = arena->chunk ? arena->chunk->room * 2 : ent_text_first_chunk;
    while (room < size)
      room *= 2;
    if (room > ent_text_most_chunk)
      room = ent_text_most_chunk;
    arena->chunk = ent_text_chunk_new(room, arena->chunk);
    arena->used = 0;
  }
  ent_text *text = (ent_text *)(arena->chunk->bytes + arena->used);
  arena->used += size;
  text->length = length;
  text->bytes[length] = 0;
  return text;
}

static void ent_text_chunks_free(ent_text_chunk *chunk) {
  while (chunk) {
    ent_text_chunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
}

// Every arena given back. (Only while no thread makes texts: when a
// schedule is done, its parallel loops are.)
static void ent_text_arenas_reset(void) {
  for (ent_text_arena *arena =
           __atomic_load_n(&ent_text_arenas, __ATOMIC_ACQUIRE);
       arena; arena = arena->next) {
    if (!arena->count)
      continue;
    if (arena->chunk) {
      ent_text_chunks_free(arena->chunk->next);
      arena->chunk->next = 0;
    }
    ent_text_chunks_free(arena->big);
    arena->big = 0;
    arena->used = 0;
    arena->count = 0;
  }
}

static long ent_text_in_arenas(void) {
  long count = 0;
  for (ent_text_arena *arena =
           __atomic_load_n(&ent_text_arenas, __ATOMIC_ACQUIRE);
       arena; arena = arena->next)
    count += arena->count;
  return count;
}

static ent_text *ent_text_make(uint32_t length) {
  if (ent_partitions_here() <= 1)
    return ent_text_cut_out(length);
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

// A text an extern fn or proc gives back: `length` bytes for C to fill
// (and a 0 after them, there already), kept as long as a joined text is.
// Any thread may make one.
ent_text *ent_text_new(uint32_t length) { return ent_text_make(length); }

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
  ent_text *held = ent_held_new(sizeof(ent_text) + length + 1);
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
  if (partitions <= 1) {
    ent_text_arenas_reset();
    return;
  }
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
  ent_text_arenas_reset();
  for (int round = 0; round < 3; ++round)
    ent_text_free(
        __atomic_exchange_n(&ent_text_made_in[round], 0, __ATOMIC_ACQUIRE));
}

void ent_text_drop(uint64_t held) {
  if (!held)
    return;
  ent_text_count(-1);
  ent_text *block = (ent_text *)(uintptr_t)held;
  ent_held_give(block, sizeof(ent_text) + block->length + 1);
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
#define ENT_TEXT_SECRET0 0x2d358dccaa6c78a5ull
#define ENT_TEXT_SECRET1 0x8bb84b93962eacc9ull
#define ENT_TEXT_SECRET2 0x4b33a62ed433d4a3ull
// (The `n` bytes at `p`, 0 to 8, as a little-endian word with 0 after
// them: in registers, by loads that overlap, not by way of the stack.)
static inline uint64_t ent_text_word(const unsigned char *p, uint64_t n) {
  uint64_t word;
  uint32_t low, high;
  if (n >= 8) {
    memcpy(&word, p, 8);
    return word;
  }
  if (n >= 4) {
    memcpy(&low, p, 4);
    memcpy(&high, p + n - 4, 4);
    return (uint64_t)low | ((uint64_t)high << (8 * (n - 4)));
  }
  if (n == 0)
    return 0;
  return (uint64_t)p[0] | ((uint64_t)p[n / 2] << (8 * (n / 2))) |
         ((uint64_t)p[n - 1] << (8 * (n - 1)));
}
// (A block of 16 bytes from `from` on: whole ones read as they are, the
// last, shorter, with 0 after it.)
static inline void ent_text_block(const ent_text *text, uint64_t from,
                                  uint64_t words[2]) {
  const unsigned char *p = (const unsigned char *)text->bytes + from;
  uint64_t left = text->length - from;
  if (left >= 16) {
    words[0] = ent_text_word(p, 8);
    words[1] = ent_text_word(p + 8, 8);
  } else if (left > 8) {
    words[0] = ent_text_word(p, 8);
    words[1] = ent_text_word(p + 8, left - 8);
  } else {
    words[0] = ent_text_word(p, left);
    words[1] = 0;
  }
}
int64_t ent_text_hash(uint64_t view, int64_t seed) {
  const ent_text *text = ent_text_of(view);
  uint64_t length = text->length, hash = (uint64_t)seed;
  for (uint64_t from = 0; from < length; from += 16) {
    uint64_t words[2];
    ent_text_block(text, from, words);
    hash = ent_text_mix(words[0] ^ ENT_TEXT_SECRET1, words[1] ^ hash);
  }
  return (int64_t)ent_text_mix(hash ^ ENT_TEXT_SECRET0,
                               length ^ ENT_TEXT_SECRET2);
}

// Two hashes of a text in one pass over its bytes, as ent_text_hash gives
// them by `seed` and by `other` (a text key's hash and its check, see
// `index ... hash`): the first, and the second kept for this thread till
// ent_text_hash_second takes it, which the compiler calls right after.
#ifdef _MSC_VER
static __declspec(thread) int64_t ent_text_second;
#else
static _Thread_local int64_t ent_text_second;
#endif
int64_t ent_text_hash_pair(uint64_t view, int64_t seed, int64_t other) {
  const ent_text *text = ent_text_of(view);
  uint64_t length = text->length, hash = (uint64_t)seed,
           more = (uint64_t)other;
  for (uint64_t from = 0; from < length; from += 16) {
    uint64_t words[2];
    ent_text_block(text, from, words);
    hash = ent_text_mix(words[0] ^ ENT_TEXT_SECRET1, words[1] ^ hash);
    more = ent_text_mix(words[0] ^ ENT_TEXT_SECRET1, words[1] ^ more);
  }
  ent_text_second = (int64_t)ent_text_mix(more ^ ENT_TEXT_SECRET0,
                                          length ^ ENT_TEXT_SECRET2);
  return (int64_t)ent_text_mix(hash ^ ENT_TEXT_SECRET0,
                               length ^ ENT_TEXT_SECRET2);
}
int64_t ent_text_hash_second(void) { return ent_text_second; }

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

// What a program calls (devices/text.ent): fns over texts, each giving a
// new text where it gives one (`ent_text_new`), or the one it was given
// where that is the same. Their results depend on their arguments alone;
// what they make is memory the program has and gives back.
#if defined(__clang__)
#define ENT_TEXT_SAME_RESULT __attribute__((annotate("ent.same_result")))
#else
#define ENT_TEXT_SAME_RESULT
#endif

// Where `what` is in `text` first, from byte `from` on; -1 where it is
// not. (An empty `what` is at `from`, where that is in the text or just
// after it.)
ENT_TEXT_SAME_RESULT int32_t ent_text_find(const ent_text *text,
                                           const ent_text *what,
                                           int32_t from) {
  text = text ? text : &ent_text_none;
  what = what ? what : &ent_text_none;
  if (from < 0)
    from = 0;
  if ((uint32_t)from > text->length)
    return -1;
  if (!what->length)
    return from;
#ifdef ENT_TEXT_STRINGZILLA
  sz_cptr_t found = sz_find(text->bytes + from, text->length - from,
                            what->bytes, what->length);
  return found ? (int32_t)(found - text->bytes) : -1;
#endif
  // (Not memmem: Windows has none.)
  const char *at = text->bytes + from, *end = text->bytes + text->length;
  while ((size_t)(end - at) >= what->length) {
    at = memchr(at, what->bytes[0], (size_t)(end - at) - what->length + 1);
    if (!at)
      return -1;
    if (!memcmp(at, what->bytes, what->length))
      return (int32_t)(at - text->bytes);
    ++at;
  }
  return -1;
}

// The bytes from `from` up to `to` (not that one), of those it has.
ENT_TEXT_SAME_RESULT const ent_text *
ent_text_slice(const ent_text *text, int32_t from, int32_t to) {
  text = text ? text : &ent_text_none;
  if (from < 0)
    from = 0;
  if (to < 0 || (uint32_t)to > text->length)
    to = to < 0 ? 0 : (int32_t)text->length;
  if (to <= from)
    return 0;
  if (from == 0 && (uint32_t)to == text->length)
    return text;
  ent_text *part = ent_text_new((uint32_t)(to - from));
  memcpy(part->bytes, text->bytes + from, (size_t)(to - from));
  return part;
}

// With A to Z as a to z; other bytes as they are.
ENT_TEXT_SAME_RESULT const ent_text *ent_text_lower(const ent_text *text) {
  text = text ? text : &ent_text_none;
  uint32_t first = 0;
  while (first < text->length &&
         !(text->bytes[first] >= 'A' && text->bytes[first] <= 'Z'))
    ++first;
  if (first == text->length)
    return text->length ? text : 0;
  ent_text *low = ent_text_new(text->length);
#ifdef ENT_TEXT_STRINGZILLA
  static char table[256];
  static int made;
  if (!made) {
    for (int c = 0; c < 256; ++c)
      table[c] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    made = 1;
  }
  sz_lookup(low->bytes, text->length, text->bytes, table);
  return low;
#endif
  for (uint32_t i = 0; i < text->length; ++i) {
    char c = text->bytes[i];
    low->bytes[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
  }
  return low;
}

// The whole number a text starts with, in decimal (after spaces and
// tabs, with a sign), as far as its digits go; 0 where it has none.
ENT_TEXT_SAME_RESULT int64_t ent_text_number(const ent_text *text) {
  text = text ? text : &ent_text_none;
  uint32_t i = 0;
  while (i < text->length && (text->bytes[i] == ' ' || text->bytes[i] == '\t'))
    ++i;
  int negative = 0;
  if (i < text->length && (text->bytes[i] == '-' || text->bytes[i] == '+'))
    negative = text->bytes[i++] == '-';
  uint64_t value = 0;
  for (; i < text->length && text->bytes[i] >= '0' && text->bytes[i] <= '9';
       ++i)
    value = value * 10 + (uint64_t)(text->bytes[i] - '0');
  return negative ? -(int64_t)value : (int64_t)value;
}
