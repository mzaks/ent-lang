add_llvm_executable(ent-lsp ent-lsp.cpp)
llvm_update_compile_flags(ent-lsp)
target_link_libraries(ent-lsp PRIVATE
  MLIREnt
  MLIREntImport
  MLIREntTransforms
  MLIRArithDialect
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRPass
  LLVMSupportLSP
)
mlir_check_all_link_libraries(ent-lsp)
