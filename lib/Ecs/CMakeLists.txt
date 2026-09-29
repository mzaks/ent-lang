add_mlir_dialect_library(MLIREcs
  EcsDialect.cpp
  EcsOps.cpp
  EcsTypes.cpp
  World.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Ecs

  DEPENDS
  MLIREcsOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRSideEffectInterfaces
)
