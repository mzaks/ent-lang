#ifndef ENT_STRUCTURE_H
#define ENT_STRUCTURE_H

#include "Ent/EntOps.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <optional>

namespace mlir::ent {

/// Whether entities stored with some components can match `query`: `holds`
/// says whether they may have a component, `always` whether they all do.
/// They hold every required component, one of each `any` group, and not
/// always a `without` component.
bool canMatch(QueryOp query, function_ref<bool(Attribute)> holds,
              function_ref<bool(Attribute)> always);

/// How `query` tests the entities of one archetype it matches: by the
/// presence of components the archetype stores optionally. The others are
/// decided for the whole archetype.
struct PresenceTest {
  /// Must be present (required components that are optional here).
  SmallVector<FlatSymbolRefAttr> present;
  /// Must be absent (`without` components that are optional here).
  SmallVector<FlatSymbolRefAttr> absent;
  /// One of each must be present (`any` groups no member of which the
  /// archetype always holds; the members it holds optionally).
  SmallVector<SmallVector<FlatSymbolRefAttr>> anyPresent;

  bool empty() const {
    return present.empty() && absent.empty() && anyPresent.empty();
  }
  /// Every component whose presence the test reads.
  SmallVector<FlatSymbolRefAttr> components() const;
};

/// Whether `query` matches `archetype` (some of its entities may match).
bool matches(QueryOp query, ArchetypeOp archetype);

/// The presence test of `query` in an archetype it matches.
PresenceTest getPresenceTest(QueryOp query, ArchetypeOp archetype);

/// The archetypes `query` matches, in declaration order.
SmallVector<ArchetypeOp> getMatchedArchetypes(QueryOp query);

/// The component `relation`'s sources (`target` false) or targets have, if
/// the relation names one and no system can take it away from an entity:
/// no query despawns entities of an archetype that holds it (which is no
/// matter for a tree that is not sorted: it drops a despawned entity's
/// edges with the entity), and none removes it. Connecting checks that both ends have their components, so
/// then every edge's end has it for as long as the edge exists, and
/// reading it through the edge needs no checks. Null otherwise.
FlatSymbolRefAttr getTrustedEndpoint(RelationOp relation, bool target);

/// The archetypes whose rows `relation`, a tree declared `sorted`, keeps
/// in its order: those that can hold its entities (they hold what its
/// sources or its targets have), in declaration order. Fails, with an
/// error on the relation, where it cannot be sorted: a system removes what
/// its sources or targets have, or another sorted tree has one of the
/// archetypes. Archetypes must have been inferred.
FailureOr<SmallVector<ArchetypeOp>> getSortedArchetypes(RelationOp relation);

/// The sorted tree that has `archetype`'s entities, or null.
RelationOp getSortingTree(ArchetypeOp archetype);

/// What `ent.add` or `ent.remove` of a component does to an entity of a
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

/// Give every spawn that lists components (`ent.spawn (@A, @B)(...)`) an
/// archetype, and add the archetypes this needs to `module`, marked
/// `inferred`. A spawn's components are the base of its archetype; a
/// component some `ent.add` can give to the archetype's entities joins it
/// as optional, and so does a listed one some `ent.remove` can take away
/// (spawns still start with it), until nothing changes: entities never
/// move between inferred archetypes. Capacity is the smallest among the
/// components that stay required, or the module's `ent.default_capacity`
/// if none has one. A declared archetype whose required components are
/// exactly a spawn's is used instead. Names join the spawn's component
/// names in declaration order (`Position_Velocity`), with `_archetype`
/// appended if that name is taken. Does nothing for spawns that already
/// have an archetype; fails, with an error, if an archetype gets no
/// capacity.
LogicalResult inferArchetypes(ModuleOp module);

/// An event a reactive query (`ent.query ... on [...]`) reacts to.
struct Trigger {
  /// Connected is no trigger a program writes: a trigger `up` a tree
  /// brings it along (see getTriggers). Its `component` is the tree, and
  /// its event that the entity was given an edge of it, which is to say
  /// another ancestor.
  enum Kind { Added, Removed, Changed, Connected };
  Kind kind;
  FlatSymbolRefAttr component;
  /// For Changed: the field, or empty for any field of the component.
  StringAttr field;
  /// The capacity the program asks for the trigger's event log (`log N`;
  /// 0 for none), if it asks.
  std::optional<int64_t> logCapacity;
  /// For `changed @C up @R`: the event is not the visited entity's but
  /// that of its ancestor along this tree, the one the query's ref
  /// `!ent.ref<@C, up @R>` leads to. `where` says whose it is otherwise:
  /// `before @R`, the sibling before's (the one a ref `before @R` leads
  /// to), or `down @R`, that of any of its children.
  FlatSymbolRefAttr via = {};
  enum Where { Own, Up, Down, Before, After };
  Where where = Own;
  /// For a trigger up a tree: how many steps up the ancestor is, or the
  /// steps to it, as the ref has them that leads to it (`hops N`, `path
  /// [...]`).
  unsigned hops = 1;
  ArrayAttr path = {};
  /// No trigger a program writes either: brought along by a trigger up a
  /// tree whose way goes to the nearest ancestor that has some components
  /// (`*`). That an entity lost one of them (Removed) or got one (Added)
  /// makes the nearest one another for those below it.
  bool onTheWay = false;
  /// Whether `ref` is the ref whose entity the trigger means.
  bool means(RefType ref) const {
    return via && ref.getVia() == via && ref.getComponent() == component &&
           ref.getIsBefore() == (where == Before) &&
           ref.getIsAfter() == (where == After) && where != Down &&
           ref.getHops() == hops && ref.getPath() == path;
  }
};

/// A step of the way from the visited entity to the one a trigger up a
/// tree means: to the parent along `tree`, or (`nearest`) to the nearest
/// ancestor along it that has all of `has`.
struct TriggerStep {
  bool nearest;
  FlatSymbolRefAttr tree;
  SmallVector<FlatSymbolRefAttr, 2> has;
  /// Or to the sibling after (1) or before (-1) along the tree.
  int sibling = 0;
};
/// The steps of a trigger of `query` that is up a tree, from the visited
/// entity on.
SmallVector<TriggerStep, 2> getSteps(const Trigger &trigger, QueryOp query);

/// The triggers of `query`, in order; empty if it is not reactive. After
/// those it names comes a Connected one for every tree some trigger of it
/// is `up` (and every tree on the way, for one that is several steps up):
/// what the entity sees through a ref up the tree has changed also when
/// the entity has been connected to another parent.
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

} // namespace mlir::ent

#endif // ENT_STRUCTURE_H
