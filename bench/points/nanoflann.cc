// The k nearest neighbours of every point of a PBBS point file by
// nanoflann (github.com/jlblancoc/nanoflann, a k-d tree of median-ish
// splits, made in one go), as bench/points/knn2.ent and knn3.ent ask
// them: each point's k nearest other points, nearest first. Each run
// makes the tree anew and asks every point (on as many threads as
// OpenMP has, the tree made on as many); prints the ms of each run and
// of it the making, and the neighbours of the first in `found.out`
// (PBBS's `-o` format) for bench/points/check.cc. bench/points/run.py
// builds and runs it.
//
//   g++ -O3 -march=native -fopenmp -std=c++17 -I nanoflann/include \
//       nanoflann.cc -o nanoflann
//   ./nanoflann points.pts k [runs] [leaf]
#include <nanoflann.hpp>

#include <omp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Cloud {
  std::vector<double> places;
  size_t dims = 0;
  size_t kdtree_get_point_count() const { return places.size() / dims; }
  double kdtree_get_pt(size_t i, size_t d) const {
    return places[i * dims + d];
  }
  template <class Box> bool kdtree_get_bbox(Box &) const { return false; }
};

template <int D>
static void neighbours(const Cloud &cloud, size_t k, int runs, size_t leaf) {
  using Tree = nanoflann::KDTreeSingleIndexAdaptor<
      nanoflann::L2_Simple_Adaptor<double, Cloud>, Cloud, D, uint32_t>;
  size_t n = cloud.kdtree_get_point_count();
  std::vector<uint32_t> found(n * k);
  unsigned threads = omp_get_max_threads();
  for (int run = 0; run < runs; ++run) {
    auto start = std::chrono::steady_clock::now();
    Tree tree(
        D, cloud,
        nanoflann::KDTreeSingleIndexAdaptorParams(
            leaf, nanoflann::KDTreeSingleIndexAdaptorFlags::None, threads));
    auto made = std::chrono::steady_clock::now();
#pragma omp parallel
    {
      std::vector<uint32_t> rows(k + 1);
      std::vector<double> far(k + 1);
#pragma omp for schedule(dynamic, 1024)
      for (size_t i = 0; i < n; ++i) {
        nanoflann::KNNResultSet<double, uint32_t> nearest(k + 1);
        nearest.init(rows.data(), far.data());
        tree.findNeighbors(nearest, &cloud.places[i * D]);
        size_t got = nearest.size(), j = 0;
        // (The point itself left out; where another is at its place and
        // comes first, the last of the k + 1.)
        for (size_t r = 0; r < got && j < k; ++r)
          if (rows[r] != i)
            found[i * k + j++] = rows[r];
      }
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    printf("nanoflann %.3f ms, made %.3f ms\n", ms(start, end),
           ms(start, made));
  }
  FILE *out = fopen("found.out", "w");
  fprintf(out, "sequenceInt\n");
  for (uint32_t id : found)
    fprintf(out, "%u\n", id);
  fclose(out);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "nanoflann points.pts k [runs] [leaf]\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  std::string header;
  in >> header;
  Cloud cloud;
  cloud.dims = header == "pbbs_sequencePoint2d" ? 2 : 3;
  double value;
  while (in >> value)
    cloud.places.push_back(value);
  size_t k = atoi(argv[2]);
  int runs = argc > 3 ? atoi(argv[3]) : 3;
  size_t leaf = argc > 4 ? atoi(argv[4]) : 10;
  if (cloud.dims == 2)
    neighbours<2>(cloud, k, runs, leaf);
  else
    neighbours<3>(cloud, k, runs, leaf);
  return 0;
}
