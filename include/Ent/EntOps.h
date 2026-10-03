#ifndef ENT_ENTOPS_H
#define ENT_ENTOPS_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/StringExtras.h"

#include "Ent/EntDialect.h"
#include "Ent/EntTypes.h"

#define GET_OP_CLASSES
#include "Ent/EntOps.h.inc"

#endif // ENT_ENTOPS_H
