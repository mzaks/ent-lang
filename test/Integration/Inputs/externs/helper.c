// What externs.c calls that its check cannot see.
#include <stdint.h>

int32_t externs_helper(int32_t x) { return x + 1; }
