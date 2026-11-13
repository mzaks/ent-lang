// The clock device (devices/clock.ent): the machine's clock. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#define _POSIX_C_SOURCE 200809L

#include "ent_extern.h"

#include <time.h>

#ifdef _WIN32
// (On Windows: the performance counter, and Sleep with the system's timer
// at 1 ms, as raylib sets it too; a wait under a millisecond is not slept.)
#include <windows.h>
#include <timeapi.h>

static double ent_clock_seconds(void) {
  static LARGE_INTEGER rate;
  LARGE_INTEGER now;
  if (!rate.QuadPart) {
    QueryPerformanceFrequency(&rate);
    timeBeginPeriod(1);
  }
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)rate.QuadPart;
}

double ent_clock_wait_until(double due) {
  double seconds = ent_clock_seconds();
  if (due > seconds) {
    DWORD ms = (DWORD)((due - seconds) * 1000.0);
    if (ms > 0)
      Sleep(ms);
    seconds = ent_clock_seconds();
  }
  return seconds;
}
#else
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
#endif

double ent_clock_wall_time(void) {
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) != TIME_UTC)
    return (double)time(NULL);
  return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

