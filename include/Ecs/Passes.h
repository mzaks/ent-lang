#ifndef ECS_PASSES_H
#define ECS_PASSES_H

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Pass/Pass.h"

namespace mlir::ecs {

#define GEN_PASS_DECL
#include "Ecs/Passes.h.inc"

#define GEN_PASS_REGISTRATION
#include "Ecs/Passes.h.inc"

} // namespace mlir::ecs

#endif // ECS_PASSES_H
