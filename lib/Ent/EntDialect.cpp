#include "Ent/EntDialect.h"
#include "Ent/EntOps.h"
#include "Ent/EntTypes.h"

using namespace mlir;
using namespace mlir::ent;

#include "Ent/EntOpsDialect.cpp.inc"

void EntDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "Ent/EntOps.cpp.inc"
      >();
  registerTypes();
}
