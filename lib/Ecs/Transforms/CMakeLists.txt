add_mlir_library(MLIREcsTransforms
  LowerToLoops.cpp

  DEPENDS
  MLIREcsPassIncGen

  LINK_LIBS PUBLIC
  MLIREcs
  MLIRArithDialect
  MLIRFuncDialect
  MLIRMemRefDialect
  MLIRSCFDialect
  MLIRPass
)
