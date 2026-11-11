#include "Ent/EntDialect.h"
#include "Ent/EntOps.h"
#include "Ent/Passes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace {
/// The module, which is all of the LLVM dialect by now, as LLVM bitcode in
/// a file: what `mlir-translate --mlir-to-llvmir` gives as text, without
/// the module being written out as text and read again for it, and the
/// LLVM IR again after that. Leaves the module empty: it has been handed
/// on, and what is left of it is not worth printing.
/// Every inttoptr from and ptrtoint to an integer of other than `bits`
/// bits made through one of `bits`, and every index of a getelementptr
/// of more bits cut to `bits` (as LLVM takes it: the same address).
static void narrowAddressCasts(llvm::Module &module, unsigned bits) {
  llvm::SmallVector<llvm::Instruction *> casts;
  for (llvm::Function &function : module)
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (llvm::isa<llvm::IntToPtrInst, llvm::PtrToIntInst,
                    llvm::GetElementPtrInst>(instruction))
        casts.push_back(&instruction);
  for (llvm::Instruction *cast : casts) {
    llvm::IRBuilder<> builder(cast);
    llvm::Type *word = builder.getIntNTy(bits);
    if (auto *at = llvm::dyn_cast<llvm::GetElementPtrInst>(cast)) {
      // (A struct's field is said by a constant i32: left as it is.)
      for (llvm::Use &index : at->indices()) {
        llvm::Type *type = index->getType();
        if (type->isIntegerTy() && type->getIntegerBitWidth() > bits)
          index.set(builder.CreateTrunc(index, word));
      }
      continue;
    }
    if (auto *to = llvm::dyn_cast<llvm::IntToPtrInst>(cast)) {
      llvm::Value *whole = to->getOperand(0);
      if (whole->getType()->getScalarSizeInBits() == bits)
        continue;
      llvm::Value *narrow = builder.CreateZExtOrTrunc(whole, word);
      to->setOperand(0, narrow);
    } else {
      auto *from = llvm::cast<llvm::PtrToIntInst>(cast);
      llvm::Type *wide = from->getType();
      if (wide->getScalarSizeInBits() == bits)
        continue;
      llvm::Value *narrow = builder.CreatePtrToInt(from->getOperand(0), word);
      llvm::Value *widened = builder.CreateZExtOrTrunc(narrow, wide);
      from->replaceAllUsesWith(widened);
      from->eraseFromParent();
    }
  }
}

/// The C library's functions the lowering declares with the index type
/// (64 bits) where C has size_t, and puts with no result where C has an
/// int: on a machine of 32-bit addresses each declared as C has it, its
/// calls given the sizes cut to 32 bits. (WebAssembly's linker calls one
/// declared otherwise than defined a stub that stops the program.)
static void declareAsC(llvm::Module &module, unsigned bits) {
  // (Which arguments are sizes.)
  static const std::pair<llvm::StringLiteral, unsigned> sized[] = {
      {"malloc", 0b1}, {"aligned_alloc", 0b11}, {"calloc", 0b11},
      {"realloc", 0b10}, {"puts", 0}};
  for (auto [name, sizes] : sized) {
    llvm::Function *old = module.getFunction(name);
    if (!old || !old->isDeclaration())
      continue;
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *word = llvm::Type::getIntNTy(context, bits);
    llvm::SmallVector<llvm::Type *> parameters;
    for (auto [index, type] : llvm::enumerate(old->getFunctionType()->params()))
      parameters.push_back((sizes >> index) & 1 ? word : type);
    llvm::Type *result = name == "puts" ? llvm::Type::getInt32Ty(context)
                                        : old->getReturnType();
    auto *type = llvm::FunctionType::get(result, parameters, false);
    if (type == old->getFunctionType())
      continue;
    old->setName(name + ".as.lowered");
    llvm::Function *c = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, name, module);
    for (llvm::User *user : llvm::make_early_inc_range(old->users())) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (!call || call->getCalledFunction() != old)
        continue;
      llvm::IRBuilder<> builder(call);
      llvm::SmallVector<llvm::Value *> arguments;
      for (auto [index, argument] : llvm::enumerate(call->args()))
        arguments.push_back((sizes >> index) & 1
                                ? builder.CreateZExtOrTrunc(argument, word)
                                : argument.get());
      llvm::CallInst *made = builder.CreateCall(c, arguments);
      made->setDebugLoc(call->getDebugLoc());
      if (!call->getType()->isVoidTy())
        call->replaceAllUsesWith(made);
      call->eraseFromParent();
    }
    if (old->use_empty())
      old->eraseFromParent();
  }
}

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
  Option<std::string> keep{
      *this, "keep",
      llvm::cl::desc("A file of the names, one to a line, that code outside "
                     "the program calls or reads: with it, every other "
                     "definition but main is made internal")};
  Option<std::string> features{
      *this, "features",
      llvm::cl::desc("The features of the machine the code is made for, as "
                     "LLVM's target-features list: with it, the functions "
                     "marked ent.no_gather have them, and prefer no "
                     "gathers (x86 only)")};
  Option<unsigned> pointerBits{
      *this, "pointer-bits", llvm::cl::init(64),
      llvm::cl::desc("How many bits the machine's addresses have: with 32 "
                     "(WebAssembly's), the casts between addresses and "
                     "whole numbers of 64 bits are made through 32")};

  void runOnOperation() final {
    mlir::ModuleOp module = getOperation();
    // (Their names: the attribute is not translated.)
    llvm::StringSet<> noGather;
    if (!features.empty())
      module.walk([&](mlir::LLVM::LLVMFuncOp func) {
        if (func->hasAttr(mlir::ent::kNoGatherAttr))
          noGather.insert(func.getSymName());
      });
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> translated =
        mlir::translateModuleToLLVMIR(module, context);
    if (!translated) {
      module.emitError("cannot be translated to LLVM IR");
      return signalPassFailure();
    }
    // The program's own functions and globals, which nothing outside it
    // names, internal: LLVM then inlines a function called from one place
    // and drops one not called, and weighs inlining others knowing that
    // no copy need be kept for others to call.
    if (!keep.empty()) {
      auto names = llvm::MemoryBuffer::getFile(keep);
      if (!names) {
        module.emitError("cannot read '")
            << keep << "': " << names.getError().message();
        return signalPassFailure();
      }
      llvm::StringSet<> kept;
      llvm::SmallVector<llvm::StringRef> lines;
      (*names)->getBuffer().split(lines, '\n', -1, false);
      for (llvm::StringRef line : lines)
        kept.insert(line.trim());
      kept.insert("main");
      for (llvm::GlobalValue &value : translated->global_values()) {
        if (value.isDeclaration() || value.hasLocalLinkage() ||
            value.getName().starts_with("llvm.") ||
            kept.contains(value.getName()))
          continue;
        if (auto *object = llvm::dyn_cast<llvm::GlobalObject>(&value))
          object->setComdat(nullptr);
        value.setLinkage(llvm::GlobalValue::InternalLinkage);
      }
    }
    // On a machine of 32-bit addresses, the program's casts between an
    // address and a whole number of 64 bits (as the lowering makes them,
    // the index type being 64 bits) through one of 32: what LLVM's
    // optimisation makes of them too, but not in the functions it leaves
    // as they are (below), whose casts WebAssembly's code generation then
    // takes as 64-bit addresses, which no WebAssembly of 32 bits loads.
    if (pointerBits != 64) {
      narrowAddressCasts(*translated, pointerBits);
      declareAsC(*translated, pointerBits);
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
    // A function's own features are all it has, not more than the
    // machine's: these, and no gathers. (The parts of it that run on
    // many threads, made into functions of their own later, have what it
    // has.)
    for (llvm::Function &function : *translated)
      if (noGather.contains(function.getName()))
        function.addFnAttr("target-features", features + ",+prefer-no-gather");
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
  // (The GPU's and NVVM's too: a program built for a GPU has its kernels
  // made into machine code for it here, see docs/gpu.md.)
  mlir::registerAllToLLVMIRTranslations(registry);

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "ent-lang optimizer driver\n", registry));
}
