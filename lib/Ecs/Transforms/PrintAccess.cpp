#include "Ecs/Access.h"
#include "Ecs/Passes.h"

namespace mlir::ecs {
#define GEN_PASS_DEF_ECSPRINTACCESS
#include "Ecs/Passes.h.inc"
} // namespace mlir::ecs

using namespace mlir;
using namespace mlir::ecs;

static std::string formatColumns(const llvm::SetVector<Column> &columns) {
  if (columns.empty())
    return "nothing";
  std::string text;
  llvm::interleave(
      columns, [&](const Column &column) { text += formatColumn(column); },
      [&] { text += ", "; });
  return text;
}

namespace {
struct EcsPrintAccess
    : public mlir::ecs::impl::EcsPrintAccessBase<EcsPrintAccess> {
  void runOnOperation() override {
    ModuleOp module = getOperation();
    SmallVector<ArchetypeOp> archetypes(module.getOps<ArchetypeOp>());
    for (SystemOp system : module.getOps<SystemOp>()) {
      SystemAccess access = computeAccess(system, archetypes);
      InFlightDiagnostic remark = system.emitRemark()
                                  << "reads " << formatColumns(access.reads)
                                  << "; writes "
                                  << formatColumns(access.writes);
      if (access.isOpaque())
        remark.attachNote(access.opaqueOp->getLoc())
            << "has effects outside component access, so the system "
               "conflicts with every other system";
    }
  }
};
} // namespace
