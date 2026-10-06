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
/// null if there are any. Imported modules are looked for next to the
/// importing file, then in `directories`, then in those the `-I` option
/// names.
OwningOpRef<ModuleOp> importEnt(llvm::SourceMgr &sourceMgr,
                                MLIRContext *context,
                                ArrayRef<std::string> directories = {});

} // namespace mlir::ent

#endif // ENT_IMPORT_H
