// Where a program's world lives: what the compiler calls for the memory of
// the world when `main` starts (devices/memory.c is linked into every
// program, as devices/sends.c is).
//
// The world is as large as its capacities say; the rows it never uses cost
// address space, not memory. It is mapped (`mmap`) without the system
// promising the memory up front, and its first pages, which the program
// sets, are the only ones used at first. The first 4 MB of each of its
// arrays are kept in small pages, the rest left to the system's huge
// pages (ent_world_pages, below).
//
// With ENT_WORLD_DIR set where the program runs, the world is a file in
// that directory (made sparse, and gone when the program ends), mapped
// shared: the system keeps in memory the pages the program goes to now and
// writes the others to the file, so a world larger than the machine's
// memory runs, slowly where it goes to much of it at once. On many ranks
// each rank's world is a file of its own.
//
// On Windows the world is memory the system commits at once (VirtualAlloc):
// committed pages count against the memory and paging file together, but
// only those the program goes to take memory. ENT_WORLD_DIR is not read
// there.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// (The world's alignment: WorldLayout::kArenaAlignment.)
#define ENT_WORLD_ALIGNMENT 16384

// (Each partition's own, where a process runs several: devices/ranks.c.)
static _Thread_local void *ent_world_base = 0;
static _Thread_local size_t ent_world_length = 0;

#ifndef _WIN32
static void ent_world_fails(const char *what) {
  fprintf(stderr, "ent: no memory for the world (%s): %s\n", what,
          strerror(errno));
  exit(1);
}
#endif

// The memory of a world of `bytes`, aligned as the world wants it.
void *ent_world_memory(int64_t bytes) {
  size_t length = (size_t)bytes + ENT_WORLD_ALIGNMENT;
#ifdef _WIN32
  void *base = VirtualAlloc(0, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!base) {
    fprintf(stderr, "ent: no memory for the world (%lld bytes): error %lu\n",
            (long long)length, (unsigned long)GetLastError());
    exit(1);
  }
#else
  const char *directory = getenv("ENT_WORLD_DIR");
  void *base;
  if (directory && *directory) {
    char path[4096];
    snprintf(path, sizeof path, "%s/ent-world.%ld.XXXXXX", directory,
             (long)getpid());
    int file = mkstemp(path);
    if (file < 0)
      ent_world_fails(path);
    // (Gone from the directory at once: the mapping keeps it while the
    // program runs.)
    unlink(path);
    if (ftruncate(file, (off_t)length) != 0)
      ent_world_fails(path);
    base = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE,
                file, 0);
    close(file);
  } else {
    base = mmap(0, length, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  }
  if (base == MAP_FAILED)
    ent_world_fails(directory && *directory ? directory : "mmap");
#endif
  ent_world_base = base;
  ent_world_length = length;
  uintptr_t at = ((uintptr_t)base + ENT_WORLD_ALIGNMENT - 1) &
                 ~(uintptr_t)(ENT_WORLD_ALIGNMENT - 1);
  return (void *)at;
}

// Which pages of the world are huge (2 MB, where the system makes them
// so: transparent huge pages): the world is told where each of its arrays
// starts (`starts`, offsets from `world`, the world's length last), and
// keeps the first ENT_SMALL_PAGES bytes of each in small pages (4 KB).
// An array the program hardly uses (an archetype of a few entities, a
// buffer of a few rows, in a world made for millions) then costs the
// pages it writes, not 2 MB each; one it goes over much has huge pages
// from there on, which a random read of it finds in fewer steps (a hash
// index's slots at 4 million keys: 9% faster than in small pages).
// ENT_HUGE_PAGES=all leaves every page to the system, =none keeps every
// page small.
#define ENT_SMALL_PAGES (4 << 20)

void ent_world_pages(void *world, const int64_t *starts, int64_t count) {
#if defined(_WIN32) || !defined(MADV_NOHUGEPAGE)
  (void)world, (void)starts, (void)count;
#else
  const char *how = getenv("ENT_HUGE_PAGES");
  const char *directory = getenv("ENT_WORLD_DIR");
  if ((how && strcmp(how, "all") == 0) || (directory && *directory) ||
      count < 1)
    return;
  uintptr_t base = (uintptr_t)world, page = 4096;
  int64_t end = starts[count - 1];
  if (how && strcmp(how, "none") == 0) {
    madvise((void *)base, (size_t)end, MADV_NOHUGEPAGE);
    return;
  }
  // (From the page an array starts in to the page its first
  // ENT_SMALL_PAGES end in, or where the next starts; arrays next to
  // each other in one call.)
  uintptr_t from = 0, to = 0;
  for (int64_t i = 0; i + 1 < count; ++i) {
    int64_t small = starts[i] + ENT_SMALL_PAGES;
    if (small > starts[i + 1])
      small = starts[i + 1];
    uintptr_t first = (base + (uintptr_t)starts[i]) & ~(page - 1);
    uintptr_t last = (base + (uintptr_t)small + page - 1) & ~(page - 1);
    if (first <= to) {
      if (last > to)
        to = last;
      continue;
    }
    if (to > from)
      madvise((void *)from, to - from, MADV_NOHUGEPAGE);
    from = first;
    to = last;
  }
  if (to > from)
    madvise((void *)from, to - from, MADV_NOHUGEPAGE);
#endif
}

#ifdef _WIN32
// (The compiler's `main` names aligned_alloc for a world without
// ent_world_memory, which never is where this file is linked; Windows's C
// library has none to name.)
void *aligned_alloc(size_t alignment, size_t size) {
  return _aligned_malloc(size, alignment);
}
#endif

// The world's memory given back when `main` ends.
// Where a unique index finds a second entity of a key (ent.index.duplicate,
// see docs/indexes.md): the entities, and the key where it is a whole
// number; and the program stops.
void ent_index_duplicate(int64_t key, int64_t has_key, int64_t a, int64_t b) {
  // (After what the program wrote out before.)
  fflush(stdout);
  if (has_key)
    fprintf(stderr,
            "ent: a unique index has two entities of one key: %lld and "
            "%lld have %lld\n",
            (long long)a, (long long)b, (long long)key);
  else
    fprintf(stderr,
            "ent: a unique index has two entities of one key: %lld and "
            "%lld\n",
            (long long)a, (long long)b);
  exit(3);
}

// Where a check the compiler made fails (no room for another row, ...;
// see --ent-stops): what the program wrote before, then why, and the
// program stops, as at a unique index's duplicate.
void ent_stop(const char *message) {
  fflush(stdout);
  fprintf(stderr, "%s\n", message);
  exit(3);
}

void ent_world_free(void *world) {
  (void)world;
  if (ent_world_base)
#ifdef _WIN32
    VirtualFree(ent_world_base, 0, MEM_RELEASE);
#else
    munmap(ent_world_base, ent_world_length);
#endif
  ent_world_base = 0;
  ent_world_length = 0;
}
