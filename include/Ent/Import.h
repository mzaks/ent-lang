#ifndef ENT_IMPORT_H
#define ENT_IMPORT_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"

namespace llvm {
class SourceMgr;
} // namespace llvm

namespace mlir::ent {

/// The files a program is made of, as the importer found them: its
/// source and those of the modules it imports (the program's first, each
/// module where it is first imported), and the files it declares that it
/// needs when it runs (`asset`), each by the name the program knows it by
/// and where it is.
struct ImportedFiles {
  std::vector<std::string> sources;
  std::vector<std::pair<std::string, std::string>> assets;
};

/// Parse ent-lang source (a `.ent` file, the main buffer of `sourceMgr`)
/// into a module of the `ent` dialect, with locations pointing into the
/// source. Reports errors through the context's diagnostics and returns
/// null if there are any. Imported modules are looked for next to the
/// importing file, then in `directories`, then in those the `-I` option
/// names. With `files`, what the program is made of is written there.
OwningOpRef<ModuleOp> importEnt(llvm::SourceMgr &sourceMgr,
                                MLIRContext *context,
                                ArrayRef<std::string> directories = {},
                                ImportedFiles *files = nullptr);

} // namespace mlir::ent

#endif // ENT_IMPORT_H
