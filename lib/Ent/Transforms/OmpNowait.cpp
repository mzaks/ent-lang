#include "Ent/Passes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTOMPNOWAIT
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// Drop the barrier after a work-sharing construct that is the last thing
/// its parallel region does: the region ends with a barrier of its own, so
/// a second one right before it only makes every thread wait twice.
template <typename OpTy>
static void dropTrailingBarrier(omp::ParallelOp parallel) {
  Block &body = parallel.getRegion().front();
  Operation *terminator = body.getTerminator();
  auto last = dyn_cast_or_null<OpTy>(terminator->getPrevNode());
  if (!last || last.getNowait())
    return;
  // Reductions and privatised values are finished at the barrier; leave
  // them alone.
  if (!last.getReductionVars().empty() || !last.getPrivateVars().empty())
    return;
  last.setNowait(true);
}

/// What a trace chose of the loop at `loc` (the `schedule` of its site,
/// see ent-lower-to-loops `tune`): "dynamic", "static", or "" for nothing.
static StringRef chosenSchedule(Location loc) {
  StringRef chosen;
  loc->walk([&](Location inner) {
    auto fused = dyn_cast<FusedLoc>(inner);
    auto marks =
        fused ? dyn_cast_or_null<DictionaryAttr>(fused.getMetadata()) : nullptr;
    if (auto schedule = marks ? marks.getAs<StringAttr>("schedule") : nullptr) {
      chosen = schedule.getValue();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return chosen;
}

struct EntOmpNowait : public mlir::ent::impl::EntOmpNowaitBase<EntOmpNowait> {
  void runOnOperation() override {
    // The body of a loop that was made parallel is in a scope for what it
    // puts on the stack. One that puts nothing there is taken out of it:
    // the canonicalizer leaves it wherever the body calls a function
    // (which might, for all it knows), and with loops of its own inside,
    // the body cannot stay in a scope when those become blocks. Nor one
    // that puts there only places of a fixed size at its start (for a
    // text a C function is given, see stackOf), which go to the start of
    // the parallel region instead: one for each thread, there once.
    SmallVector<memref::AllocaScopeOp> scopes;
    getOperation()->walk([&](memref::AllocaScopeOp scope) {
      if (isa_and_nonnull<omp::LoopNestOp>(scope->getParentOp()) &&
          scope->getNumResults() == 0 && scope.getRegion().hasOneBlock())
        scopes.push_back(scope);
    });
    for (memref::AllocaScopeOp scope : scopes) {
      auto parallel = scope->getParentOfType<omp::ParallelOp>();
      Block &start = scope.getRegion().front();
      SmallVector<memref::AllocaOp> hoisted;
      bool allocates = false;
      scope->walk([&](Operation *op) {
        auto alloca = dyn_cast<memref::AllocaOp>(op);
        if (alloca && parallel && parallel.getRegion().hasOneBlock() &&
            alloca->getBlock() == &start && alloca->getNumOperands() == 0 &&
            alloca.getType().hasStaticShape())
          hoisted.push_back(alloca);
        else
          allocates |= isa<memref::AllocaOp, LLVM::AllocaOp>(op);
      });
      if (allocates)
        continue;
      for (memref::AllocaOp alloca : hoisted)
        alloca->moveBefore(&parallel.getRegion().front(),
                           parallel.getRegion().front().begin());
      Block &body = scope.getRegion().front();
      scope->getBlock()->getOperations().splice(
          Block::iterator(scope), body.getOperations(), body.begin(),
          std::prev(body.end()));
      scope->erase();
    }
    // A loop over a few pieces (a query's 256 slices, say) whose rows have
    // loops of their own, so that a piece may take much longer than
    // another (an entity's edges, a filter that lets few through), deals
    // its pieces one at a time to whichever thread is free: on cores of
    // two speeds, and with work that is not even, a fixed share each
    // leaves the threads that are done waiting for the slowest. A loop
    // without inner loops keeps fixed shares: each thread then has the
    // same rows in every loop, in its own cache. (Each piece runs on one
    // thread either way: what the program does is the same.)
    getOperation()->walk([](omp::WsloopOp loop) {
      auto nest = dyn_cast<omp::LoopNestOp>(loop.getWrappedLoop());
      if (!nest || nest.getLoopUpperBounds().size() != 1)
        return;
      auto upper =
          nest.getLoopUpperBounds()[0].getDefiningOp<arith::ConstantIndexOp>();
      if (!upper || upper.value() > 1024)
        return;
      // (Where a trace chose, as it chose: of a few pieces too.)
      StringRef chosen = chosenSchedule(loop.getLoc());
      if (!chosen.empty()) {
        if (chosen == "dynamic")
          loop.setScheduleKind(omp::ClauseScheduleKind::Dynamic);
        return;
      }
      bool uneven = false;
      nest->walk([&](LoopLikeOpInterface inner) {
        auto outer = inner->getParentOfType<LoopLikeOpInterface>();
        uneven |= outer && outer != nest;
      });
      if (uneven)
        loop.setScheduleKind(omp::ClauseScheduleKind::Dynamic);
    });
    getOperation()->walk([](omp::ParallelOp parallel) {
      if (!parallel.getRegion().hasOneBlock())
        return;
      dropTrailingBarrier<omp::WsloopOp>(parallel);
      dropTrailingBarrier<omp::SectionsOp>(parallel);
    });
  }
};

} // namespace
