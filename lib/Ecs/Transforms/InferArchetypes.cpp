#include "Ecs/Passes.h"
#include "Ecs/Structure.h"

namespace mlir::ecs {
#define GEN_PASS_DEF_ECSINFERARCHETYPES
#include "Ecs/Passes.h.inc"
} // namespace mlir::ecs

using namespace mlir;
using namespace mlir::ecs;

namespace {
struct EcsInferArchetypes
    : public mlir::ecs::impl::EcsInferArchetypesBase<EcsInferArchetypes> {
  void runOnOperation() override {
    if (failed(inferArchetypes(getOperation())))
      signalPassFailure();
  }
};
} // namespace
