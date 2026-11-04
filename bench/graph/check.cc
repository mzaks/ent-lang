// The PageRank loop below is adapted from gapbs's src/pr_spmv.cc
// (github.com/sbeamer/gapbs, commit 941462c), under its license:
//
// Copyright (c) 2015, The Regents of the University of California (Regents).
// All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the Regents nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL REGENTS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// What bench/graph/graph.ent must find on a graph, by GAP's own graph
// builder (github.com/sbeamer/gapbs, its src/ on the include path): its
// pr_spmv PageRank on one core, and the depth of every vertex from the
// source by a plain breadth-first search. Prints the lines graph.ent
// prints, without the times:
//
//   pr <iterations> iterations, error <e>, hash <h>
//   bfs <reached> reached, depths <sum of depths>
//
// The hash adds every score's bits times the vertex's number plus one.
//
//   g++ -O2 -ffp-contract=off -std=c++11 -I gapbs/src check.cc -o check
//   ./check -f graph.el -r <source>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

typedef float ScoreT;
const float kDamp = 0.85;

int main(int argc, char *argv[]) {
  CLApp cli(argc, argv, "check");
  if (!cli.ParseArgs())
    return -1;
  Builder b(cli);
  Graph g = b.MakeGraph();

  // pr_spmv's PageRankPull, on one core.
  const ScoreT init_score = 1.0f / g.num_nodes();
  const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
  pvector<ScoreT> scores(g.num_nodes(), init_score);
  pvector<ScoreT> outgoing_contrib(g.num_nodes());
  int iters = 0;
  double error = 0;
  for (int iter = 0; iter < 20; iter++) {
    error = 0;
    for (NodeID n = 0; n < g.num_nodes(); n++)
      outgoing_contrib[n] = scores[n] / g.out_degree(n);
    for (NodeID u = 0; u < g.num_nodes(); u++) {
      ScoreT incoming_total = 0;
      for (NodeID v : g.in_neigh(u))
        incoming_total += outgoing_contrib[v];
      ScoreT old_score = scores[u];
      scores[u] = base_score + kDamp * incoming_total;
      error += fabs(scores[u] - old_score);
    }
    iters++;
    if (error < 1e-4)
      break;
  }
  uint64_t hash = 0;
  for (NodeID n = 0; n < g.num_nodes(); n++) {
    uint32_t bits;
    memcpy(&bits, &scores[n], 4);
    hash += uint64_t(bits) * uint64_t(n + 1);
  }
  printf("pr %d iterations, error %f, hash %llu\n", iters, error,
         (unsigned long long)hash);

  // Depths from the source.
  NodeID source = cli.start_vertex() < 0 ? 0 : cli.start_vertex();
  std::vector<int> depth(g.num_nodes(), -1);
  std::vector<NodeID> queue{source};
  depth[source] = 0;
  for (size_t i = 0; i < queue.size(); i++)
    for (NodeID v : g.out_neigh(queue[i]))
      if (depth[v] < 0) {
        depth[v] = depth[queue[i]] + 1;
        queue.push_back(v);
      }
  int64_t reached = 0, depths = 0;
  for (int d : depth)
    if (d >= 0) {
      reached++;
      depths += d;
    }
  printf("bfs %lld reached, depths %lld\n", (long long)reached,
         (long long)depths);
  return 0;
}
