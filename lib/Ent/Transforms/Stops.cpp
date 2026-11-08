#include "Ent/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "llvm/ADT/StringMap.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTSTOPS
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

struct EntStops : public mlir::ent::impl::EntStopsBase<EntStops> {
  void runOnOperation() override {
    ModuleOp module = getOperation();
    MLIRContext *context = module.getContext();
    SmallVector<cf::AssertOp> asserts;
    module.walk([&](cf::AssertOp assert) {
      // (On the GPU there is no C library to call: left as it is.)
      if (!assert->getParentOfType<gpu::GPUModuleOp>() &&
          !assert->getParentOfType<gpu::GPUFuncOp>())
        asserts.push_back(assert);
    });
    if (asserts.empty())
      return;
    OpBuilder builder(context);
    auto pointer = LLVM::LLVMPointerType::get(context);
    builder.setInsertionPointToStart(module.getBody());
    // (devices/memory.c, linked into every program.)
    auto stop = dyn_cast_or_null<LLVM::LLVMFuncOp>(
        SymbolTable::lookupSymbolIn(module, "ent_stop"));
    if (!stop)
      stop = LLVM::LLVMFuncOp::create(
          builder, module.getLoc(), "ent_stop",
          LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(context),
                                      {pointer}));
    stop->setAttr("passthrough",
                  builder.getArrayAttr({builder.getStringAttr("noreturn")}));
    // Each message once, as a C string.
    llvm::StringMap<LLVM::GlobalOp> said;
    for (cf::AssertOp assert : asserts) {
      StringRef message = assert.getMsg();
      LLVM::GlobalOp &global = said[message];
      if (!global) {
        std::string text = ("ent: " + message).str();
        text.push_back('\0');
        auto type = LLVM::LLVMArrayType::get(IntegerType::get(context, 8),
                                             text.size());
        builder.setInsertionPointToStart(module.getBody());
        global = LLVM::GlobalOp::create(
            builder, assert.getLoc(), type, /*isConstant=*/true,
            LLVM::Linkage::Private,
            "ent_stop_said_" + std::to_string(said.size()),
            builder.getStringAttr(text));
      }
      // Where it fails: the program says so on stderr, after what it
      // wrote to stdout, and stops (the assert after it is not reached).
      builder.setInsertionPoint(assert);
      Location loc = assert.getLoc();
      Value fails = arith::XOrIOp::create(
          builder, loc, assert.getArg(),
          arith::ConstantIntOp::create(builder, loc, 1, 1));
      auto when = scf::IfOp::create(builder, loc, fails);
      builder.setInsertionPointToStart(when.thenBlock());
      Value address = LLVM::AddressOfOp::create(builder, loc, global);
      LLVM::CallOp::create(builder, loc, stop, ValueRange{address});
    }
  }
};

} // namespace
