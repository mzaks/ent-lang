// The C of proc_thread.ent: counts the calls made from threads other than
// the main one (the thread that ran the constructors).
#include "ent_extern.h"

#include <pthread.h>
#include <stdio.h>

static pthread_t main_thread;
static long outside_off, inside_off, inside_all, pure_off;

__attribute__((constructor)) static void start(void) {
  main_thread = pthread_self();
}

static int off_main(void) {
  return !pthread_equal(pthread_self(), main_thread);
}

int64_t ent_outside(void) {
  if (off_main()) __atomic_add_fetch(&outside_off, 1, __ATOMIC_RELAXED);
  return 1;
}

int64_t ent_inside(int64_t n) {
  __atomic_add_fetch(&inside_all, 1, __ATOMIC_RELAXED);
  if (off_main()) __atomic_add_fetch(&inside_off, 1, __ATOMIC_RELAXED);
  return n % 3;
}

// (Vouched for: what it counts is not what it gives.)
ENT_SAME_RESULT int64_t ent_pure(int64_t n) {
  if (off_main()) __atomic_add_fetch(&pure_off, 1, __ATOMIC_RELAXED);
  return n % 5;
}

void ent_report(void) {
  printf("procs off the main thread: outside %ld, inside %ld of %ld\n",
         outside_off, inside_off, inside_all);
  printf("fn off the main thread: %s\n", pure_off ? "some" : "none");
}
