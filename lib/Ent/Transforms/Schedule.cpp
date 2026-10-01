#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTSCHEDULE
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// Access of every system in the module, computed once.
class AccessCache {
public:
  AccessCache(ModuleOp module)
      : symbols(module), archetypes(module.getOps<ArchetypeOp>()) {}

  const SystemAccess &get(RunOp run) {
    auto system = symbols.lookup<SystemOp>(run.getSystem());
    auto [it, inserted] = cache.try_emplace(system);
    if (inserted)
      it->second = computeAccess(system, archetypes);
    return it->second;
  }

private:
  SymbolTable symbols;
  SmallVector<ArchetypeOp> archetypes;
  llvm::DenseMap<Operation *, SystemAccess> cache;
};

} // namespace

/// Stage the runs of one barrier-free segment of a schedule, inserting the
/// stages before `insertionPoint`. Each run goes into the stage right after
/// the latest stage holding a run it conflicts with.
static void stageSegment(IRRewriter &rewriter, MutableArrayRef<RunOp> runs,
                         Operation *insertionPoint, AccessCache &accesses,
                         bool explain) {
  if (runs.empty())
    return;

  SmallVector<unsigned> level(runs.size(), 0);
  unsigned numStages = 0;
  for (auto [j, later] : llvm::enumerate(runs)) {
    const SystemAccess &laterAccess = accesses.get(later);
    std::optional<std::string> reason;
    RunOp waitsFor;
    for (unsigned i = 0; i < j; ++i) {
      if (level[i] + 1 <= level[j])
        continue;
      std::optional<std::string> conflict = laterAccess.conflictWith(
          accesses.get(runs[i]), runs[i].getSystem());
      if (!conflict)
        continue;
      level[j] = level[i] + 1;
      reason = conflict;
      waitsFor = runs[i];
    }
    numStages = std::max(numStages, level[j] + 1);
    if (explain && reason)
      later.emitRemark("@")
          << later.getSystem() << " waits for @" << waitsFor.getSystem()
          << ": " << *reason;
  }

  Location loc = runs.front().getLoc();
  for (unsigned stageIndex = 0; stageIndex < numStages; ++stageIndex) {
    rewriter.setInsertionPoint(insertionPoint);
    auto stage = StageOp::create(rewriter, loc);
    StageOp::ensureTerminator(stage.getBody(), rewriter, loc);
    Operation *terminator = stage.getBody().front().getTerminator();
    for (auto [run, runLevel] : llvm::zip(runs, level))
      if (runLevel == stageIndex)
        run->moveBefore(terminator);
  }
}

namespace {
struct EntSchedule : public mlir::ent::impl::EntScheduleBase<EntSchedule> {
  using EntScheduleBase::EntScheduleBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    AccessCache accesses(module);
    IRRewriter rewriter(module.getContext());

    for (ScheduleOp schedule : module.getOps<ScheduleOp>()) {
      Block &body = schedule.getBody().front();
      if (llvm::any_of(body, [](Operation &op) { return isa<StageOp>(op); }))
        continue;

      SmallVector<RunOp> segment;
      for (Operation &op : llvm::make_early_inc_range(body)) {
        if (auto run = dyn_cast<RunOp>(op)) {
          segment.push_back(run);
          continue;
        }
        // Everything staged so far must happen before a barrier or the end.
        if (op.hasTrait<OpTrait::IsTerminator>() || !isMemoryEffectFree(&op)) {
          stageSegment(rewriter, segment, &op, accesses, explain);
          segment.clear();
        }
      }
    }
  }
};
} // namespace
