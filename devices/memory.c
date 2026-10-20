// Where a program's world lives: what the compiler calls for the memory of
// the world when `main` starts (devices/memory.c is linked into every
// program, as devices/sends.c is).
//
// The world is as large as its capacities say; the rows it never uses cost
// address space, not memory. It is mapped (`mmap`) without the system
// promising the memory up front, and its first pages, which the program
// sets, are the only ones used at first.
//
// With ENT_WORLD_DIR set where the program runs, the world is a file in
// that directory (made sparse, and gone when the program ends), mapped
// shared: the system keeps in memory the pages the program goes to now and
// writes the others to the file, so a world larger than the machine's
// memory runs, slowly where it goes to much of it at once. On many ranks
// each rank's world is a file of its own.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// (The world's alignment: WorldLayout::kArenaAlignment.)
#define ENT_WORLD_ALIGNMENT 16384

static void *ent_world_base = 0;
static size_t ent_world_length = 0;

static void ent_world_fails(const char *what) {
  fprintf(stderr, "ent: no memory for the world (%s): %s\n", what,
          strerror(errno));
  exit(1);
}

// The memory of a world of `bytes`, aligned as the world wants it.
void *ent_world_memory(int64_t bytes) {
  size_t length = (size_t)bytes + ENT_WORLD_ALIGNMENT;
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
  ent_world_base = base;
  ent_world_length = length;
  uintptr_t at = ((uintptr_t)base + ENT_WORLD_ALIGNMENT - 1) &
                 ~(uintptr_t)(ENT_WORLD_ALIGNMENT - 1);
  return (void *)at;
}

// The world's memory given back when `main` ends.
void ent_world_free(void *world) {
  (void)world;
  if (ent_world_base)
    munmap(ent_world_base, ent_world_length);
  ent_world_base = 0;
  ent_world_length = 0;
}
