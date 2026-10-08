get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
get_property(extension_libs GLOBAL PROPERTY MLIR_EXTENSION_LIBS)

set(LLVM_LINK_COMPONENTS BitWriter Core Support)
add_llvm_executable(ent-opt ent-opt.cpp)
llvm_update_compile_flags(ent-opt)
target_link_libraries(ent-opt PRIVATE
  ${dialect_libs}
  ${conversion_libs}
  ${extension_libs}
  MLIREnt
  MLIREntTransforms
  MLIROptLib
  MLIRRegisterAllDialects
  MLIRRegisterAllExtensions
  MLIRRegisterAllPasses
  MLIRTargetLLVMIRExport
  MLIRBuiltinToLLVMIRTranslation
  MLIRLLVMToLLVMIRTranslation
  MLIROpenMPToLLVMIRTranslation
)
mlir_check_all_link_libraries(ent-opt)
