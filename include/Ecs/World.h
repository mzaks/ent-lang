#ifndef ECS_WORLD_H
#define ECS_WORLD_H

#include "Ecs/EcsOps.h"

#include "llvm/ADT/DenseMap.h"

namespace mlir::ecs {

/// One field of one component in one archetype's table.
struct WorldColumn {
  StringAttr component;
  StringAttr field;
  Type type;
  /// Byte offset of the column's first element in the world arena.
  uint64_t offset;
};

struct WorldArchetype {
  ArchetypeOp op;
  int64_t capacity;
  /// Position of the archetype's entity count in the counts array.
  unsigned index;
  SmallVector<WorldColumn> columns;

  /// The column of `component`.`field`, or null.
  const WorldColumn *find(StringAttr component, StringAttr field) const;
};

/// Static layout of the whole world in one arena, computed from the
/// archetypes' components and capacities.
///
/// The arena starts with one i64 entity count per archetype, in declaration
/// order. Columns follow, one per field, ordered by archetype, then by the
/// archetype's components, then by the component's fields. Each column
/// starts at a 64-byte boundary, pushed 17 cache lines beyond the end of
/// the previous column: columns laid out back to back from a page-aligned
/// base would otherwise tend to start at the same cache set, which costs a
/// single core 7-8% on the example (see bench/RESULTS.md).
struct WorldLayout {
  static constexpr uint64_t kArenaAlignment = 16384;
  static constexpr uint64_t kColumnAlignment = 64;
  static constexpr uint64_t kStagger = 17 * 64;

  SmallVector<WorldArchetype> archetypes;
  /// Bytes taken by the counts at the start of the arena.
  uint64_t countsBytes = 0;
  /// Total size of the arena.
  uint64_t totalBytes = 0;

  /// Computes the layout, or emits an error on `module` and fails if a
  /// field has a type the world cannot store.
  static FailureOr<WorldLayout> compute(ModuleOp module);
};

/// Bytes one element of `type` takes in world storage, or 0 if the type is
/// not supported (i1, i8, i16, i32, i64, index, f32 and f64 are).
uint64_t getStorageBytes(Type type);

} // namespace mlir::ecs

#endif // ECS_WORLD_H
