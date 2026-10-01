add_llvm_executable(ent-translate ent-translate.cpp)
llvm_update_compile_flags(ent-translate)
target_link_libraries(ent-translate PRIVATE
  MLIREnt
  MLIRArithDialect
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRTranslateLib
)
mlir_check_all_link_libraries(ent-translate)
