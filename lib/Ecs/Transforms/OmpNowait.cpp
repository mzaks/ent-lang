#include "Ecs/Passes.h"

namespace mlir::ecs {
#define GEN_PASS_DEF_ECSOMPNOWAIT
#include "Ecs/Passes.h.inc"
} // namespace mlir::ecs

using namespace mlir;
using namespace mlir::ecs;

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

struct EcsOmpNowait : public mlir::ecs::impl::EcsOmpNowaitBase<EcsOmpNowait> {
  void runOnOperation() override {
    getOperation()->walk([](omp::ParallelOp parallel) {
      if (!parallel.getRegion().hasOneBlock())
        return;
      dropTrailingBarrier<omp::WsloopOp>(parallel);
      dropTrailingBarrier<omp::SectionsOp>(parallel);
    });
  }
};

} // namespace
