add_mlir_library(MLIREntTransforms
  Access.cpp
  GpuKernelNames.cpp
  InferArchetypes.cpp
  LowerToLoops.cpp
  LowerViews.cpp
  OmpNowait.cpp
  PrintAccess.cpp
  Schedule.cpp

  DEPENDS
  MLIREntPassIncGen

  LINK_LIBS PUBLIC
  MLIREnt
  MLIRArithDialect
  MLIRControlFlowDialect
  MLIRFuncDialect
  MLIRFuncTransforms
  MLIRGPUDialect
  MLIRGPUTransforms
  MLIRLLVMDialect
  MLIRMemRefDialect
  MLIROpenMPDialect
  MLIRSCFDialect
  MLIRSCFTransforms
  MLIRTransforms
  MLIRPass
  MLIRSideEffectInterfaces
)
