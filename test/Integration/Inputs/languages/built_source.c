#include "ent_extern.h"

#include <math.h>

// The square root comes from the library built.link names.
int32_t ent_built_double_it(int32_t n) {
  volatile double four = 4.0;
  return n * (int32_t)sqrt(four);
}
