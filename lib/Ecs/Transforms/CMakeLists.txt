add_mlir_library(MLIREcsTransforms
  Access.cpp
  InferArchetypes.cpp
  LowerToLoops.cpp
  OmpNowait.cpp
  PrintAccess.cpp
  Schedule.cpp

  DEPENDS
  MLIREcsPassIncGen

  LINK_LIBS PUBLIC
  MLIREcs
  MLIRArithDialect
  MLIRControlFlowDialect
  MLIRFuncDialect
  MLIRFuncTransforms
  MLIRMemRefDialect
  MLIROpenMPDialect
  MLIRSCFDialect
  MLIRSCFTransforms
  MLIRTransforms
  MLIRPass
  MLIRSideEffectInterfaces
)
