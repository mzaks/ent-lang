#include "Ent/Passes.h"

#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/IR/SymbolTable.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTGPUKERNELNAMES
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// Kernels named as PTX names things: `gpu-kernel-outlining` names a
/// kernel after the function it was in, which for a system of a module is
/// `module.system`, and PTX takes no `.` in a name.
struct EntGpuKernelNames
    : public mlir::ent::impl::EntGpuKernelNamesBase<EntGpuKernelNames> {
  void runOnOperation() override {
    ModuleOp module = getOperation();
    SmallVector<gpu::GPUFuncOp> kernels;
    module.walk([&](gpu::GPUFuncOp kernel) {
      if (kernel.getName().contains('.'))
        kernels.push_back(kernel);
    });
    for (gpu::GPUFuncOp kernel : kernels) {
      std::string name = kernel.getName().str();
      std::replace(name.begin(), name.end(), '.', '_');
      auto container = kernel->getParentOfType<gpu::GPUModuleOp>();
      // (The launches name it inside its module: @module::@kernel.)
      auto from = SymbolRefAttr::get(
          container.getNameAttr(),
          {FlatSymbolRefAttr::get(kernel.getNameAttr())});
      auto to = SymbolRefAttr::get(
          container.getNameAttr(),
          {FlatSymbolRefAttr::get(&getContext(), name)});
      module.walk([&](gpu::LaunchFuncOp launch) {
        if (launch.getKernel() == from)
          launch.setKernelAttr(to);
      });
      SymbolTable::setSymbolName(kernel, name);
    }
    // (Roots rounded to the nearest, as IEEE 754 and the CPU have them.)
    SmallVector<math::SqrtOp> roots;
    module.walk([&](gpu::GPUModuleOp container) {
      container.walk([&](math::SqrtOp root) { roots.push_back(root); });
    });
    for (math::SqrtOp root : roots) {
      Type type = root.getType();
      if (!type.isF32() && !type.isF64())
        continue;
      OpBuilder builder(root);
      auto exact = LLVM::CallIntrinsicOp::create(
          builder, root.getLoc(), type,
          builder.getStringAttr(type.isF32() ? "llvm.nvvm.sqrt.rn.f"
                                             : "llvm.nvvm.sqrt.rn.d"),
          ValueRange{root.getOperand()});
      root.replaceAllUsesWith(exact.getResult(0));
      root.erase();
    }
    // (fma as LLVM's own, `fma.rn` in the PTX: the lowering to NVVM would
    // call libdevice's, which only a build that finds CUDA links.)
    SmallVector<math::FmaOp> fmas;
    module.walk([&](gpu::GPUModuleOp container) {
      container.walk([&](math::FmaOp fma) { fmas.push_back(fma); });
    });
    for (math::FmaOp fma : fmas) {
      Type type = fma.getType();
      if (!type.isF32() && !type.isF64())
        continue;
      OpBuilder builder(fma);
      auto fused = LLVM::FMAOp::create(builder, fma.getLoc(), fma.getA(),
                                       fma.getB(), fma.getC());
      fma.replaceAllUsesWith(fused.getResult());
      fma.erase();
    }
  }
};

} // namespace
