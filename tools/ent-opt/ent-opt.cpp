#include "Ent/EntDialect.h"
#include "Ent/Passes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/OpenMP/OpenMPToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace {
/// The module, which is all of the LLVM dialect by now, as LLVM bitcode in
/// a file: what `mlir-translate --mlir-to-llvmir` gives as text, without
/// the module being written out as text and read again for it, and the
/// LLVM IR again after that. Leaves the module empty: it has been handed
/// on, and what is left of it is not worth printing.
struct EmitLLVM : mlir::PassWrapper<EmitLLVM, mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(EmitLLVM)
  EmitLLVM() = default;
  EmitLLVM(const EmitLLVM &other) : PassWrapper(other) {}
  llvm::StringRef getArgument() const final { return "ent-emit-llvm"; }
  llvm::StringRef getDescription() const final {
    return "Write the module, lowered to the LLVM dialect, as LLVM bitcode";
  }
  Option<std::string> output{*this, "output",
                             llvm::cl::desc("The bitcode file to write")};

  void runOnOperation() final {
    mlir::ModuleOp module = getOperation();
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> translated =
        mlir::translateModuleToLLVMIR(module, context);
    if (!translated) {
      module.emitError("cannot be translated to LLVM IR");
      return signalPassFailure();
    }
    // What makes the world runs once: it is not worth the time it takes
    // to make it fast, nor is what only it calls.
    llvm::SmallPtrSet<llvm::Function *, 16> once;
    for (llvm::Function &function : *translated)
      if (!function.isDeclaration() &&
          (function.getName() == "world_setup" ||
           function.getName().ends_with(".world_setup")))
        once.insert(&function);
    for (bool grew = !once.empty(); grew;) {
      grew = false;
      for (llvm::Function &function : *translated) {
        if (function.isDeclaration() || once.count(&function) ||
            !function.hasLocalLinkage() || function.use_empty())
          continue;
        bool only = llvm::all_of(function.users(), [&](llvm::User *user) {
          auto *call = llvm::dyn_cast<llvm::CallBase>(user);
          return call && call->getCalledFunction() == &function &&
                 once.count(call->getFunction());
        });
        if (only)
          grew |= once.insert(&function).second;
      }
    }
    for (llvm::Function *function : once) {
      function->addFnAttr(llvm::Attribute::OptimizeNone);
      function->addFnAttr(llvm::Attribute::NoInline);
    }
    std::error_code error;
    llvm::raw_fd_ostream file(output, error, llvm::sys::fs::OF_None);
    if (error) {
      module.emitError("cannot write '") << output << "': " << error.message();
      return signalPassFailure();
    }
    llvm::WriteBitcodeToFile(*translated, file);
    while (!module.getBody()->empty())
      module.getBody()->back().erase();
  }
};
} // namespace

int main(int argc, char **argv) {
  mlir::registerAllPasses();
  mlir::ent::registerEntPasses();
  mlir::PassRegistration<EmitLLVM>();

  mlir::DialectRegistry registry;
  registry.insert<mlir::ent::EntDialect>();
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
  mlir::registerOpenMPDialectTranslation(registry);

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "ent-lang optimizer driver\n", registry));
}
