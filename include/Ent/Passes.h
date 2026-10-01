#ifndef ENT_PASSES_H
#define ENT_PASSES_H

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Pass/Pass.h"

namespace mlir::ent {

#define GEN_PASS_DECL
#include "Ent/Passes.h.inc"

#define GEN_PASS_REGISTRATION
#include "Ent/Passes.h.inc"

} // namespace mlir::ent

#endif // ENT_PASSES_H
