// The clock device (devices/clock.ent). Compiled with the program,
// against its generated header, which the build names ent_world.h. All
// its state is in the world.

#define _POSIX_C_SOURCE 200809L

#include "ent_world.h"

#include <time.h>

static double ent_clock_seconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

void ent_clock_tick(ent_world *world) {
  double *epoch = ent_clock_Epoch_value(world);
  double *now = ent_clock_Time_now(world);
  int64_t *frame = ent_clock_Time_frame(world);
  double seconds = ent_clock_seconds();
  if (*frame == 0) {
    *epoch = seconds;
  } else if (*ent_clock_FrameRate_value(world) > 0) {
    // Frame n is due n periods after the first; no drift adds up.
    double due = *epoch + (double)*frame / *ent_clock_FrameRate_value(world);
    if (due > seconds) {
      double wait = due - seconds;
      struct timespec left = {(time_t)wait,
                              (long)((wait - (double)(time_t)wait) * 1e9)};
      while (nanosleep(&left, &left) != 0) {
      }
      seconds = ent_clock_seconds();
    }
  }
  double elapsed = seconds - *epoch;
  *ent_clock_Time_dt(world) = *frame == 0 ? 0 : (float)(elapsed - *now);
  *now = elapsed;
  ++*frame;
}
