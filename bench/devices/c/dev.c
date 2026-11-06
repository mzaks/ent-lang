// The workloads of bench/devices in C, plainly written: what dev.ent
// declares.
#include "ent_extern.h"

#include <string.h>

uint64_t ent_dev_mix(uint64_t x) {
  uint64_t z = x + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

int64_t ent_dev_square_sum(int32_t n) {
  int64_t squares[n > 0 ? n : 1];
  for (int32_t i = 0; i < n; i++) squares[i] = (int64_t)i * i;
  int64_t sum = 0;
  for (int32_t i = 0; i < n; i++) sum += squares[i];
  return sum;
}

uint64_t ent_dev_median16(uint64_t seed) {
  uint64_t a[16];
  for (int k = 0; k < 16; k++) a[k] = ent_dev_mix(seed * 16 + k);
  for (int i = 1; i < 16; i++) {
    uint64_t v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
  return a[7];
}

int64_t ent_dev_parse(int32_t length, const uint8_t *text, int32_t *count,
                      int32_t room, int64_t *into) {
  int64_t n = 0, current = 0;
  int digits = 0;
  for (int32_t i = 0; i < length; i++) {
    uint8_t c = text[i];
    if (c == ',') {
      if (n == room) { *count = (int32_t)n; return -2; }
      into[n++] = current;
      current = 0;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      digits = 1;
    } else {
      *count = 0;
      return -1;
    }
  }
  if (digits) {
    if (n == room) { *count = (int32_t)n; return -2; }
    into[n++] = current;
  }
  *count = (int32_t)n;
  return n;
}

int64_t ent_dev_median(int32_t n, const int64_t *values, int32_t *count,
                       int32_t room, int64_t *s) {
  if (n > room) n = room;
  memcpy(s, values, (size_t)n * sizeof *s);
  *count = n;
  if (n == 0) return 0;
  int64_t k = (n - 1) / 2, lo = 0, hi = n - 1;
  while (lo < hi) {
    int64_t p = s[(lo + hi) / 2], i = lo, j = hi;
    while (i <= j) {
      while (s[i] < p) i++;
      while (s[j] > p) j--;
      if (i <= j) {
        int64_t t = s[i]; s[i] = s[j]; s[j] = t;
        i++; j--;
      }
    }
    if (k <= j) hi = j;
    else if (k >= i) lo = i;
    else break;
  }
  return s[k];
}

void ent_dev_blur(int32_t n, const uint8_t *from, int32_t *count,
                  int32_t room, uint8_t *to, int32_t width) {
  if (n > room) n = room;
  int32_t height = n / width;
  for (int32_t y = 0; y < height; y++)
    for (int32_t x = 0; x < width; x++) {
      int32_t at = y * width + x;
      if (y == 0 || x == 0 || y == height - 1 || x == width - 1) {
        to[at] = from[at];
      } else {
        int sum = 0;
        for (int dy = -1; dy <= 1; dy++) {
          int32_t row = at + dy * width;
          sum += from[row - 1] + from[row] + from[row + 1];
        }
        to[at] = (uint8_t)(sum / 9);
      }
    }
  *count = height * width;
}
