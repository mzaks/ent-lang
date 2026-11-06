// The C of ranks_procs.ent. What each process saw, said as it ends (on
// standard error): how often `daily` and `tally` were called, from how
// many threads, whether one was the process's main thread, and how many
// calls of `tally` there were at once at most.
#include "ent_extern.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static pthread_t main_thread;
static long daily_calls, tally_calls, on_main, now, most;
static pthread_t threads[64];
static int nthreads;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor)) static void start(void) {
  main_thread = pthread_self();
}

static void seen(void) {
  pthread_mutex_lock(&lock);
  int known = 0;
  for (int i = 0; i < nthreads; i++)
    known |= pthread_equal(threads[i], pthread_self());
  if (!known && nthreads < 64) threads[nthreads++] = pthread_self();
  if (pthread_equal(pthread_self(), main_thread)) on_main++;
  pthread_mutex_unlock(&lock);
}

int64_t ent_daily(void) {
  seen();
  __atomic_add_fetch(&daily_calls, 1, __ATOMIC_RELAXED);
  return 1;
}

int64_t ent_tally(int32_t n) {
  seen();
  __atomic_add_fetch(&tally_calls, 1, __ATOMIC_RELAXED);
  long at = __atomic_add_fetch(&now, 1, __ATOMIC_RELAXED);
  long high = __atomic_load_n(&most, __ATOMIC_RELAXED);
  while (at > high && !__atomic_compare_exchange_n(&most, &high, at, 0,
                                                   __ATOMIC_RELAXED,
                                                   __ATOMIC_RELAXED)) {
  }
  usleep(5);
  __atomic_sub_fetch(&now, 1, __ATOMIC_RELAXED);
  return n % 3;
}

int64_t ent_twice(int32_t n) { return 2 * (int64_t)n; }

__attribute__((destructor)) static void end(void) {
  fprintf(stderr, "calls: daily %ld tally %ld, threads %d, on main %ld, "
                  "at once %ld\n",
          daily_calls, tally_calls, nthreads, on_main, most);
}
