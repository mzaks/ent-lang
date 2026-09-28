get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
get_property(extension_libs GLOBAL PROPERTY MLIR_EXTENSION_LIBS)

add_llvm_executable(ecs-opt ecs-opt.cpp)
llvm_update_compile_flags(ecs-opt)
target_link_libraries(ecs-opt PRIVATE
  ${dialect_libs}
  ${conversion_libs}
  ${extension_libs}
  MLIREcs
  MLIROptLib
  MLIRRegisterAllDialects
  MLIRRegisterAllPasses
)
mlir_check_all_link_libraries(ecs-opt)
