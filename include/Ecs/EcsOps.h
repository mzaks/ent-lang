#ifndef ECS_ECSOPS_H
#define ECS_ECSOPS_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "Ecs/EcsDialect.h"
#include "Ecs/EcsTypes.h"

#define GET_OP_CLASSES
#include "Ecs/EcsOps.h.inc"

#endif // ECS_ECSOPS_H
