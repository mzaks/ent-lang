#ifndef ECS_STRUCTURE_H
#define ECS_STRUCTURE_H

#include "Ecs/EcsOps.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <optional>

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

/// An event a reactive query (`ecs.query ... on [...]`) reacts to.
struct Trigger {
  enum Kind { Added, Removed, Changed };
  Kind kind;
  FlatSymbolRefAttr component;
  /// For Changed: the field, or empty for any field of the component.
  StringAttr field;
  /// The capacity the program asks for the trigger's event log (`log N`;
  /// 0 for none), if it asks.
  std::optional<int64_t> logCapacity;
};

/// The triggers of `query`, in order; empty if it is not reactive.
SmallVector<Trigger> getTriggers(QueryOp query);

/// What the world records per row so that reactive queries can find the
/// entities with an event: the tick of the entity's last event of one kind
/// (for Changed, of one field, or of any field if the field is empty).
struct Stamp {
  Trigger::Kind kind;
  StringAttr component;
  StringAttr field;

  bool operator==(const Stamp &other) const {
    return kind == other.kind && component == other.component &&
           field == other.field;
  }
};

/// The stamp a trigger reads.
Stamp getStamp(const Trigger &trigger);

/// Which archetypes store which stamps. A stamp means something in an
/// archetype that holds the component (added, changed) or may lack it
/// (removed: does not hold it, or holds it optionally). It is stored there
/// only if a reactive query observing it can see the archetype's entities:
/// the query matches the archetype, or entities can move from it, through
/// archetypes where the stamp means something, into one the query matches.
/// Stamps move with their entities; a move into an archetype that stores a
/// stamp its source does not is the event itself (it gains or loses the
/// component).
class StampPlan {
public:
  static StampPlan compute(ModuleOp module);

  /// Every stamp some reactive query observes, in the order the queries
  /// and their triggers appear.
  ArrayRef<Stamp> getStamps() const { return stamps; }
  /// Whether rows of `archetype` store `stamp`.
  bool stores(ArchetypeOp archetype, const Stamp &stamp) const;

private:
  SmallVector<Stamp> stamps;
  /// Per stamp (same order), the archetypes storing it.
  SmallVector<llvm::SmallPtrSet<Operation *, 4>> storing;
};

} // namespace mlir::ecs

#endif // ECS_STRUCTURE_H
