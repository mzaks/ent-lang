//===- Fusion.h - Which runs of a schedule are fused, and why --*- C++ -*-===//
//
// ent-lower-to-loops fuses every maximal sequence of runs of entity-local
// systems of a schedule into one loop per archetype (`fuse-systems`, which
// `ent build` always asks for). This is its plan, for tools that show it
// (ent-fusion-map): the same that the lowering follows.
//
//===----------------------------------------------------------------------===//

#ifndef ENT_FUSION_H
#define ENT_FUSION_H

#include "Ent/EntOps.h"
#include <optional>
#include <string>
#include <vector>

namespace mlir {
namespace ent {

/// A run of a schedule (or another op of it that ends a sequence) and the
/// group of fused runs it is in, from 0 in the schedule's order (-1: in
/// none, and `why` says why it cannot be fused with any other); where it
/// starts a group and could not join the one before, `why` says why not.
/// `at` is what the group's loops are placed before (and whose place they
/// are given, which a trace names them by): what ends the group.
struct FusionStep {
  Operation *op;
  int group;
  std::string why;
  Operation *at = nullptr;
};

/// The fusion of `schedule`'s runs, its stages dissolved (as the lowering
/// does), its archetypes inferred: for a world without a GPU and ranks.
std::vector<FusionStep> planScheduleFusion(ScheduleOp schedule);

/// Why the loops of `query` always run on one thread (in a build with
/// --parallel too), or none where they may run on many: those of an
/// archetype that may hold `parallel-min-entities` or more (a million,
/// unless the build says fewer), from that many on, each time it runs.
std::optional<std::string> whyOneThread(QueryOp query);

} // namespace ent
} // namespace mlir

#endif // ENT_FUSION_H
