add_mlir_dialect(EntOps ent)

set(LLVM_TARGET_DEFINITIONS Passes.td)
mlir_tablegen(Passes.h.inc -gen-pass-decls -name Ent)
add_public_tablegen_target(MLIREntPassIncGen)
