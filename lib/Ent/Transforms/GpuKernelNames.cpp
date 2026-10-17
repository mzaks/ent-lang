#include "Ent/Passes.h"

#include "mlir/Dialect/GPU/IR/GPUDialect.h"
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
  }
};

} // namespace
