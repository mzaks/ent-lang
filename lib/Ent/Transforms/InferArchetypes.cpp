#include "Ent/Passes.h"
#include "Ent/Structure.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTINFERARCHETYPES
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {
struct EntInferArchetypes
    : public mlir::ent::impl::EntInferArchetypesBase<EntInferArchetypes> {
  void runOnOperation() override {
    if (failed(inferArchetypes(getOperation())))
      signalPassFailure();
  }
};
} // namespace
