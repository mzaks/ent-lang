// The clock device (devices/clock.ent): the machine's clock. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#define _POSIX_C_SOURCE 200809L

#include "ent_extern.h"

#include <time.h>

static double ent_clock_seconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

double ent_clock_wait_until(double due) {
  double seconds = ent_clock_seconds();
  if (due > seconds) {
    double wait = due - seconds;
    struct timespec left = {(time_t)wait,
                            (long)((wait - (double)(time_t)wait) * 1e9)};
    while (nanosleep(&left, &left) != 0) {
    }
    seconds = ent_clock_seconds();
  }
  return seconds;
}
