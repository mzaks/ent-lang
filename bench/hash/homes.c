// Where keys land in a hash index's slots (bench/RESULTS.md, 2026-10-23):
// the slots looked at past a key's home as keys go in, by the home from
// the low bits or the high bits of the hash, of one or two multiplies,
// or SplitMix64's finalizer, for runs of whole numbers and strides.
//   clang -O2 homes.c -o homes && ./homes
#include <stdint.h>
#include <stdio.h>
typedef unsigned __int128 u128;
static uint64_t mum(uint64_t a, uint64_t b) {
  u128 r = (u128)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}
static uint64_t one(uint64_t x) {
  return mum(x ^ 0x2d358dccaa6c78a5ull,
             0xbdd89aa982704029ull ^ 0x8bb84b93962eacc9ull);
}
static uint64_t two(uint64_t x) {
  uint64_t A = x ^ 0x2d358dccaa6c78a5ull,
           B = 0xbdd89aa982704029ull ^ 0x8bb84b93962eacc9ull;
  u128 r = (u128)A * B;
  A = (uint64_t)r;
  B = (uint64_t)(r >> 64);
  return mum(A ^ 0x2d358dccaa6c78a5ull, B ^ 0x8bb84b93962eacc9ull);
}
static uint64_t split(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
static int used[1 << 20];
static void run(const char *name, uint64_t (*h)(uint64_t), int high, int n,
                int size, int stride) {
  int bits = __builtin_ctz(size);
  for (int i = 0; i < size; i++)
    used[i] = 0;
  long steps = 0;
  int most = 0;
  for (int k = 0; k < n; k++) {
    uint64_t v = h((uint64_t)k * stride);
    unsigned s =
        high ? (unsigned)(v >> (64 - bits)) : (unsigned)(v & (size - 1));
    int c = 0;
    while (used[s]) {
      s = (s + 1) & (size - 1);
      c++;
    }
    used[s] = 1;
    steps += c;
    if (c > most)
      most = c;
  }
  printf("  %-22s mean %.2f longest %d\n", name, (double)steps / n, most);
}
int main() {
  int cases[][3] = {{1000, 2048, 1},
                    {7, 64, 1},
                    {100000, 262144, 1},
                    {1000, 2048, 1024},
                    {30000, 65536, 1}};
  for (int c = 0; c < 5; c++) {
    printf("%d keys (stride %d) in %d slots\n", cases[c][0], cases[c][2],
           cases[c][1]);
    run("one mum, low bits", one, 0, cases[c][0], cases[c][1], cases[c][2]);
    run("one mum, high bits", one, 1, cases[c][0], cases[c][1], cases[c][2]);
    run("two mums, low bits", two, 0, cases[c][0], cases[c][1], cases[c][2]);
    run("splitmix, low bits", split, 0, cases[c][0], cases[c][1], cases[c][2]);
  }
}
