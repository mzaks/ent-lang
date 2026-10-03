#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTPRINTACCESS
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

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
struct EntPrintAccess
    : public mlir::ent::impl::EntPrintAccessBase<EntPrintAccess> {
  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    SmallVector<ArchetypeOp> archetypes(module.getOps<ArchetypeOp>());
    for (Operation &system : module.getOps()) {
      if (!isa<SystemOp, ExternOp>(system))
        continue;
      SystemAccess access = computeAccess(&system, archetypes);
      // An extern system without a contract is opaque as a whole.
      if (access.opaqueOp == &system) {
        system.emitRemark() << "declares no access, so the system conflicts "
                               "with every other system";
        continue;
      }
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
