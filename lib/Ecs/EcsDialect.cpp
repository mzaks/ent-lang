#include "Ecs/EcsDialect.h"
#include "Ecs/EcsOps.h"
#include "Ecs/EcsTypes.h"

using namespace mlir;
using namespace mlir::ecs;

#include "Ecs/EcsOpsDialect.cpp.inc"

void EcsDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "Ecs/EcsOps.cpp.inc"
      >();
  registerTypes();
}
