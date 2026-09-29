add_mlir_library(MLIREcsTransforms
  Access.cpp
  LowerToLoops.cpp
  PrintAccess.cpp
  Schedule.cpp

  DEPENDS
  MLIREcsPassIncGen

  LINK_LIBS PUBLIC
  MLIREcs
  MLIRArithDialect
  MLIRControlFlowDialect
  MLIRFuncDialect
  MLIRMemRefDialect
  MLIROpenMPDialect
  MLIRSCFDialect
  MLIRPass
  MLIRSideEffectInterfaces
)
