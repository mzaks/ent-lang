add_llvm_executable(ecs-translate ecs-translate.cpp)
llvm_update_compile_flags(ecs-translate)
target_link_libraries(ecs-translate PRIVATE
  MLIREcs
  MLIRArithDialect
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRTranslateLib
)
mlir_check_all_link_libraries(ecs-translate)
