// absl::flat_hash_map (a SwissTable) by the loop of bench/hash/get.ent
// (bench/RESULTS.md, 2026-10-23): KEYS keys, in a row or at random (as
// get.ent's `key`), each looked up by a number at random 4,000,000 times
// a run, eight runs, found or not; the same loop without the lookups
// taken off. ns a lookup, the best of five, with absl's hash and with the
// index's (rapidhash's mix).
//
//   clang++ -O2 -DNDEBUG -march=native -std=c++17 bench/hash/swiss.cpp \
//     -o swiss $(pkg-config --libs absl_hash absl_raw_hash_set)
//   taskset -c 2 ./swiss
#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

typedef unsigned __int128 u128;
static inline uint64_t wymix(uint64_t a, uint64_t b) {
  u128 r = (u128)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}
static inline uint64_t wyrand(uint64_t key, int64_t n) {
  uint64_t s = key + ((uint64_t)n + 1) * 0x2d358dccaa6c78a5ull;
  return wymix(s, s ^ 0x8bb84b93962eacc9ull);
}
struct RapidHash {
  size_t operator()(int64_t k) const {
    return wymix((uint64_t)k ^ 0x2d358dccaa6c78a5ull,
                 0xbdd89aa982704029ull ^ 0x8bb84b93962eacc9ull);
  }
};

static int64_t key(int64_t i, bool random) {
  return random ? (int64_t)(wyrand(1, i) >> 1) : i;
}

template <class Map>
static double run(const Map &map, int64_t keys, bool random, int64_t miss,
                  bool ask) {
  double best = 1e30;
  for (int round = 0; round < 5; ++round) {
    auto start = std::chrono::steady_clock::now();
    int64_t total = 0;
    for (uint64_t step = 1; step <= 8; ++step) {
      int64_t n = 0;
      for (int64_t i = 0; i < 4000000; ++i) {
        int64_t k = key((int64_t)(wyrand(step, i) % (uint64_t)keys), random) +
                    miss * (keys + 1);
        if (ask)
          n += map.find(k) != map.end();
        else
          n += k != 7;
      }
      total += n;
    }
    auto end = std::chrono::steady_clock::now();
    static volatile int64_t sink;
    sink = total;
    best = std::min(
        best, std::chrono::duration<double, std::nano>(end - start).count());
  }
  return best / 32e6;
}

int main() {
  for (int64_t keys : {1000, 200000, 4000000}) {
    for (bool random : {false, true}) {
      absl::flat_hash_map<int64_t, int32_t, RapidHash> rapid;
      absl::flat_hash_map<int64_t, int32_t> own;
      rapid.reserve(keys);
      own.reserve(keys);
      for (int64_t i = 0; i < keys; ++i) {
        rapid[key(i, random)] = (int32_t)i;
        own[key(i, random)] = (int32_t)i;
      }
      double ns[2][2];
      for (int miss = 0; miss < 2; ++miss) {
        double base = run(own, keys, random, miss, false);
        ns[0][miss] = run(rapid, keys, random, miss, true) - base;
        ns[1][miss] = run(own, keys, random, miss, true) - base;
      }
      printf("%7lld keys %-9s | found: rapid %5.2f, absl %5.2f | not: rapid "
             "%5.2f, absl %5.2f ns\n",
             (long long)keys, random ? "at random" : "in a row", ns[0][0],
             ns[1][0], ns[0][1], ns[1][1]);
    }
  }
}
