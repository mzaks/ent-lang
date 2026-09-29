#ifndef ECS_WORLD_H
#define ECS_WORLD_H

#include "Ecs/EcsOps.h"

#include "llvm/ADT/DenseMap.h"

namespace mlir::ecs {

/// One field of one component in one archetype's table.
struct WorldColumn {
  StringAttr component;
  /// Empty for the presence byte of an optional component.
  StringAttr field;
  Type type;
  /// Byte offset of the column's first element in the world arena.
  uint64_t offset;
};

/// A move of entities out of an archetype, into `target`, caused by adding
/// or removing `component`.
struct WorldMove {
  ArchetypeOp target;
  StringAttr component;
  bool add;
  /// The action code pending rows use for this move (despawn is 0).
  unsigned code;
  /// For an add: one column per field of the component, holding the values
  /// to initialise it with, by pending slot.
  SmallVector<WorldColumn> values;
};

struct WorldArchetype {
  ArchetypeOp op;
  int64_t capacity;
  /// Position of the archetype's entity count in the counts array.
  unsigned index;
  SmallVector<WorldColumn> columns;
  /// The id of the entity in each row (i64).
  uint64_t idOffset = 0;
  /// For an archetype that some query despawns from or moves entities out
  /// of: the rows to remove at the end of such a query (one i32 per slot of
  /// capacity) and where their number is kept. Zero if there is none.
  uint64_t pendingOffset = 0;
  uint64_t pendingCountOffset = 0;
  /// With moves: what to do with each pending row (0 despawn, otherwise a
  /// move's code), one i32 per slot.
  uint64_t pendingActionOffset = 0;
  SmallVector<WorldMove> moves;

  bool hasPending() const { return pendingOffset != 0; }

  /// The move for adding or removing `component`, or null.
  const WorldMove *findMove(StringAttr component, bool add) const;

  /// The column of `component`.`field`, or null.
  const WorldColumn *find(StringAttr component, StringAttr field) const;
};

/// One field of a resource: stored once.
struct WorldResourceField {
  StringAttr field;
  Type type;
  /// Byte offset of the field in the world arena.
  uint64_t offset;
};

struct WorldResource {
  ResourceOp op;
  SmallVector<WorldResourceField> fields;

  /// The field named `field`, or null.
  const WorldResourceField *find(StringAttr field) const;
};

/// Static layout of the whole world in one arena, computed from the
/// archetypes' components and capacities and from the resources.
///
/// The arena starts with one i64 entity count per archetype, in declaration
/// order. Resources follow, each starting on a cache line, with naturally
/// aligned fields. Columns follow, one per field, ordered by archetype,
/// then by the archetype's components, then by the component's fields; an
/// optional component's fields are followed by its presence column (one
/// i8 per entity, 1 if present). Each column starts at a 64-byte boundary,
/// pushed 17 cache lines beyond the end of the previous column: columns
/// laid out back to back from a page-aligned base would otherwise tend to
/// start at the same cache set, which costs a single core 7-8% on the
/// example (see bench/RESULTS.md). Each archetype's columns are followed by
/// its id column; an archetype that entities are despawned from or moved
/// out of also gets a pending counter in the header and pending lists after
/// its id column. The entity table comes last.
struct WorldLayout {
  static constexpr uint64_t kArenaAlignment = 16384;
  static constexpr uint64_t kColumnAlignment = 64;
  static constexpr uint64_t kStagger = 17 * 64;

  SmallVector<WorldArchetype> archetypes;
  SmallVector<WorldResource> resources;

  /// The entity table, indexed by an id's slot (its lower 32 bits): the
  /// slot's generation (i32), the archetype index and row it lives at (i32
  /// each), and a stack of free slots (i32). One entry per slot of total
  /// capacity. The next never-used slot and the number of free slots are
  /// i64 counters in the header.
  int64_t entityCapacity = 0;
  uint64_t nextSlotOffset = 0;
  uint64_t freeCountOffset = 0;
  uint64_t generationOffset = 0;
  uint64_t locationArchetypeOffset = 0;
  uint64_t locationRowOffset = 0;
  uint64_t freeListOffset = 0;
  /// Bytes taken by the counts at the start of the arena.
  uint64_t countsBytes = 0;
  /// Bytes taken by the counts and resources, which a new world zeroes.
  uint64_t headerBytes = 0;
  /// Total size of the arena.
  uint64_t totalBytes = 0;

  /// Computes the layout, or emits an error on `module` and fails if a
  /// field has a type the world cannot store.
  static FailureOr<WorldLayout> compute(ModuleOp module);

  /// The layout of `resource`; it must be declared in the module.
  const WorldResource &getResource(StringAttr resource) const;
};

/// Bytes one element of `type` takes in world storage, or 0 if the type is
/// not supported (i1, i8, i16, i32, i64, index, f32 and f64 are).
uint64_t getStorageBytes(Type type);

} // namespace mlir::ecs

#endif // ECS_WORLD_H
