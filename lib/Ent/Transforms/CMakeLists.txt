add_mlir_library(MLIREntTransforms
  Access.cpp
  GpuKernelNames.cpp
  Indexes.cpp
  InferArchetypes.cpp
  LowerToLoops.cpp
  LowerViews.cpp
  OmpNowait.cpp
  PrintAccess.cpp
  Schedule.cpp
  Trace.cpp

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
  MLIRMathDialect
  MLIRMemRefDialect
  MLIROpenMPDialect
  MLIRSCFDialect
  MLIRSCFTransforms
  MLIRTransforms
  MLIRVectorDialect
  MLIRPass
  MLIRSideEffectInterfaces
)
