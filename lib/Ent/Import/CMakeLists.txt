add_mlir_library(MLIREntImport
  Lexer.cpp
  Parser.cpp

  LINK_LIBS PUBLIC
  MLIREnt
  MLIRArithDialect
  MLIRSCFDialect
  MLIRIR
)
