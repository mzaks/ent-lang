#ifndef ECS_STRUCTURE_H
#define ECS_STRUCTURE_H

#include "Ecs/EcsOps.h"

namespace mlir::ecs {

/// The archetypes `query` matches (those holding all of its components,
/// always or optionally), in declaration order.
SmallVector<ArchetypeOp> getMatchedArchetypes(QueryOp query);

/// What `ecs.add` or `ecs.remove` of a component does to an entity of a
/// given archetype. The storage decides, not the program: the same op sets
/// a presence byte where the component is optional and moves the entity
/// to another archetype where it is not.
struct ComponentChange {
  enum Kind {
    /// The component is optional here: set or clear its presence.
    Presence,
    /// Adding a component the archetype always has: overwrite its fields.
    Overwrite,
    /// Removing a component the archetype does not hold: nothing happens.
    Nothing,
    /// Move the entity to `target`, the archetype with exactly the
    /// resulting components.
    Move,
    /// A move would be needed, but no archetype has exactly the resulting
    /// components.
    NoTarget,
  };
  Kind kind;
  ArchetypeOp target;

  bool isStructural() const { return kind == Move; }
};

ComponentChange classifyChange(ArchetypeOp archetype,
                               FlatSymbolRefAttr component, bool add);

} // namespace mlir::ecs

#endif // ECS_STRUCTURE_H
