add_mlir_dialect(EcsOps ecs)

set(LLVM_TARGET_DEFINITIONS Passes.td)
mlir_tablegen(Passes.h.inc -gen-pass-decls -name Ecs)
add_public_tablegen_target(MLIREcsPassIncGen)
