add_mlir_dialect_library(MLIREnt
  EntDialect.cpp
  EntOps.cpp
  EntTypes.cpp
  Structure.cpp
  World.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Ent

  DEPENDS
  MLIREntOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRLoopLikeInterface
  MLIRSideEffectInterfaces
)
