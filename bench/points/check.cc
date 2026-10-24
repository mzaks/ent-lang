// Whether the neighbours found of a PBBS point file are the k nearest:
// for points drawn at random, a scan over all the others for their k
// nearest (by the squared distance, nearest first), held against what the
// file of neighbours says (PBBS's `-o` file: a line "sequenceInt", then
// k lines for each point). PBBS's own neighborsCheck holds only the
// first of the k so.
//
//   g++ -O2 -std=c++17 check.cc -o check
//   ./check points.pts found.out k [samples] [but-last]
//
// `but-last` leaves the last point out: PBBS's neighborsTime.C writes the
// neighbours of all points but that one (its loop goes to n - 1).
//
// Prints "<wrong> of <samples> points wrong" and gives 1 where any is.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static std::string slurp(const char *name) {
  std::ifstream in(name, std::ios::binary);
  if (!in) {
    fprintf(stderr, "no file %s\n", name);
    exit(2);
  }
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "check points.pts found.out k [samples] [but-last]\n");
    return 2;
  }
  std::string points = slurp(argv[1]);
  std::string found = slurp(argv[2]);
  int k = atoi(argv[3]);
  long samples = argc > 4 ? atol(argv[4]) : 1000;
  bool butLast = argc > 5 && !strcmp(argv[5], "but-last");

  const char *p = points.c_str();
  const char *line = strchr(p, '\n');
  int dims = line[-2] - '0'; // "..Point3d"
  std::vector<double> xyz;
  char *end;
  for (const char *at = line + 1;;) {
    double v = strtod(at, &end);
    if (end == at)
      break;
    xyz.push_back(v);
    at = end;
  }
  long n = xyz.size() / dims;

  std::vector<long> ids;
  const char *f = strchr(found.c_str(), '\n') + 1;
  for (;;) {
    long v = strtol(f, &end, 10);
    if (end == f)
      break;
    ids.push_back(v);
    f = end;
  }
  if ((long)ids.size() != n * k) {
    printf("%zu neighbours where %ld points of %d want %ld\n", ids.size(), n, k,
           n * k);
    return 1;
  }

  auto dist = [&](long a, long b) {
    double d = 0;
    for (int s = 0; s < dims; s++) {
      double t = xyz[b * dims + s] - xyz[a * dims + s];
      d += t * t;
    }
    return d;
  };
  long wrong = 0;
  uint64_t h = 88172645463325252ull;
  std::vector<std::pair<double, long>> all(n);
  for (long i = 0; i < samples; i++) {
    h ^= h << 13;
    h ^= h >> 7;
    h ^= h << 17;
    long a = h % (butLast ? n - 1 : n);
    for (long b = 0; b < n; b++)
      all[b] = {b == a ? 1e308 : dist(a, b), b};
    std::partial_sort(all.begin(), all.begin() + k, all.end());
    for (int j = 0; j < k; j++) {
      long b = ids[a * k + j];
      if (b < 0 || b >= n || b == a || dist(a, b) != all[j].first) {
        if (wrong < 5)
          printf("point %ld, neighbour %d: %ld at %.17g where %ld at %.17g\n",
                 a, j, b, b >= 0 && b < n ? dist(a, b) : -1.0, all[j].second,
                 all[j].first);
        wrong++;
        break;
      }
    }
  }
  printf("%ld of %ld points wrong\n", wrong, samples);
  return wrong ? 1 : 0;
}
