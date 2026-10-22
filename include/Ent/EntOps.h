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

namespace mlir::ent {

/// How the row an `ent.buffer.set` in a query writes is its entity's own:
/// at a place the same for every entity (`base`, terms added or, where
/// the flag says, taken away), plus what a running sum gave the entity
/// (`running`: so its interval is [that, that + the value it summed)),
/// plus `offset` (null: none), inside that interval. Where it is not
/// shown that the offset is inside (from 0 up to the value, or the value
/// at least 1 where there is none), or that the value summed is never
/// below nought (else intervals overlap), the lowering asks as it runs.
struct OwnRow {
  RunningOp running;
  SmallVector<std::pair<Value, bool>> base;
  Value offset;
  bool offsetShown = false;
  bool valueShown = false;
};

/// The OwnRow of `set`, a write in a query (not one that takes turns), or
/// failure with what it is not, in `why`.
FailureOr<OwnRow> ownRowOf(BufferSetOp set, std::string &why);

/// Whether `a` and `b` are made the same way, of what is the same for
/// every entity of a query: so two writes with them are at one base.
bool sameValue(Value a, Value b);

/// Marks an `ent.buffer.at` or `ent.buffer.set` of a window (`w: mut
/// Cohorts[start, count]` in a `for`): a row the visited entity took with
/// `ent.buffer.claim`, its number already inside the window.
inline constexpr llvm::StringLiteral kWindowAttr = "ent.window";

/// Marks the loop of a `near` by blocks of rows (the parser writes it), and
/// ent-lower-to-loops the function it ends up in: one that ent-emit-llvm,
/// given the features of the machine, has prefer no gathers. LLVM gathers
/// the coordinates of each row within reach that the loop visits, which
/// takes longer than loading them one by one (bench/RESULTS.md,
/// 2026-10-22).
inline constexpr llvm::StringLiteral kNoGatherAttr = "ent.no_gather";

/// In a grid index's `Name_tangled` (the parser writes it): where its rows
/// are so out of order that they are put in order anew, more than one in
/// this many below the one before. ent-lower-to-loops puts in 64, or what
/// `tune` chose of a trace (by `tangled Name`).
inline constexpr int64_t kTangledPlaceholder = 918273646;

} // namespace mlir::ent

#endif // ENT_ENTOPS_H
