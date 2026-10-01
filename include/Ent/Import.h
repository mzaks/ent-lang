#ifndef ENT_IMPORT_H
#define ENT_IMPORT_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"

namespace llvm {
class SourceMgr;
} // namespace llvm

namespace mlir::ent {

/// Parse ent-lang source (a `.ent` file, the main buffer of `sourceMgr`)
/// into a module of the `ent` dialect, with locations pointing into the
/// source. Reports errors through the context's diagnostics and returns
/// null if there are any.
OwningOpRef<ModuleOp> importEnt(llvm::SourceMgr &sourceMgr,
                                MLIRContext *context);

} // namespace mlir::ent

#endif // ENT_IMPORT_H
