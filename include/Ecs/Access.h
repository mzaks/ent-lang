#ifndef ECS_ACCESS_H
#define ECS_ACCESS_H

#include "Ecs/EcsOps.h"

#include "llvm/ADT/SetVector.h"
#include <optional>
#include <string>
#include <tuple>

namespace mlir::ecs {

/// One field of one component in one archetype's table, or one field of a
/// resource: the unit of storage that systems read and write. Held as
/// (archetype name, component or resource name, field name); the archetype
/// is null for a resource, the field is empty for the presence of an
/// optional component, the component is empty for an archetype's entity
/// count (empty field) and id column (field "id"), and all three are empty
/// for the entity table.
using Column = std::tuple<StringAttr, StringAttr, StringAttr>;

/// Prints a column as `Archetype.Component.field`, `Resource.field`,
/// `Archetype.Component?` for a presence, `Archetype.count`,
/// `Archetype.id`, or `entities` for the entity table.
std::string formatColumn(const Column &column);

/// The columns a system actually reads and writes, derived from the
/// `ecs.get`/`ecs.set` ops in its queries and its `ecs.read`/`ecs.write`
/// ops rather than from its declared `reads` and `writes`. Binding a
/// component without accessing it only filters archetypes and is not an
/// access.
struct SystemAccess {
  llvm::SetVector<Column> reads;
  llvm::SetVector<Column> writes;
  /// The first op with effects outside component access (e.g. a call), or
  /// null. A system with such an op conflicts with every other system.
  Operation *opaqueOp = nullptr;

  bool isOpaque() const { return opaqueOp != nullptr; }

  /// Why running this system and `other` (named `otherName`) in either
  /// order could give different results, as a clause about this system
  /// ("it reads X, which @other writes"), or nothing if they commute.
  std::optional<std::string> conflictWith(const SystemAccess &other,
                                          StringRef otherName) const;
};

/// Computes the access of `system` against the module's archetypes.
SystemAccess computeAccess(SystemOp system, ArrayRef<ArchetypeOp> archetypes);

/// True if `op` has memory effects of its own or unknown effects. Nested
/// ops of region-holding ops with recursive effects are not considered.
bool hasOwnEffects(Operation *op);

} // namespace mlir::ecs

#endif // ECS_ACCESS_H
