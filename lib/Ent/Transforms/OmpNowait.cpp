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

struct EntOmpNowait : public mlir::ent::impl::EntOmpNowaitBase<EntOmpNowait> {
  void runOnOperation() override {
    // The body of a loop that was made parallel is in a scope for what it
    // puts on the stack. One that puts nothing there is taken out of it:
    // the canonicalizer leaves it wherever the body calls a function
    // (which might, for all it knows), and with loops of its own inside,
    // the body cannot stay in a scope when those become blocks.
    SmallVector<memref::AllocaScopeOp> scopes;
    getOperation()->walk([&](memref::AllocaScopeOp scope) {
      if (isa_and_nonnull<omp::LoopNestOp>(scope->getParentOp()) &&
          scope->getNumResults() == 0 && scope.getRegion().hasOneBlock())
        scopes.push_back(scope);
    });
    for (memref::AllocaScopeOp scope : scopes) {
      bool allocates = false;
      scope->walk([&](Operation *op) {
        allocates |= isa<memref::AllocaOp, LLVM::AllocaOp>(op);
      });
      if (allocates)
        continue;
      Block &body = scope.getRegion().front();
      scope->getBlock()->getOperations().splice(
          Block::iterator(scope), body.getOperations(), body.begin(),
          std::prev(body.end()));
      scope->erase();
    }
    getOperation()->walk([](omp::ParallelOp parallel) {
      if (!parallel.getRegion().hasOneBlock())
        return;
      dropTrailingBarrier<omp::WsloopOp>(parallel);
      dropTrailingBarrier<omp::SectionsOp>(parallel);
    });
  }
};

} // namespace
