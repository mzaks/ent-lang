#include <map>
#include "Ent/Access.h"
#include "Ent/EntOps.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"
#include "Ent/World.h"

#include "mlir/IR/Dominance.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/Transforms/Patterns.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTLOWERTOLOOPS
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

static MemRefType getArenaType(MLIRContext *context,
                               const WorldLayout &layout) {
  return MemRefType::get({static_cast<int64_t>(layout.totalBytes)},
                         IntegerType::get(context, 8));
}

namespace {

/// Access to the world inside one lowered function. Entity counts and
/// column views are created at the function's entry the first time they
/// are needed, so a function only carries what it uses.
class WorldAccess {
public:
  WorldAccess(IRRewriter &rewriter, const WorldLayout &layout, Value arena)
      : rewriter(rewriter), layout(layout), arena(arena) {}

  Value getArena() const { return arena; }
  /// How many entities each archetype has, by its index.
  Value counts() { return getCounts(); }
  bool hasIds() const { return layout.entities.hasIds(); }

  /// The type a value of `type` is stored as: entity ids become integers
  /// of the width the layout chose; everything else is stored as is.
  Type storageType(Type type) {
    if (isa<EntityType>(type))
      return rewriter.getIntegerType(layout.entities.idBits);
    if (auto text = dyn_cast<TextType>(type))
      return text.getStorageType();
    if (auto named = dyn_cast<EnumType>(type))
      return named.getStorageType();
    return type;
  }
  /// Convert a value to and from its stored form. Entity ids cross with a
  /// cast that the final type conversion removes.
  Value toStorage(Location loc, Value value) {
    Type type = storageType(value.getType());
    if (type == value.getType())
      return value;
    return UnrealizedConversionCastOp::create(rewriter, loc, type, value)
        .getResult(0);
  }
  Value fromStorage(Location loc, Value stored, Type type) {
    if (stored.getType() == type)
      return stored;
    return UnrealizedConversionCastOp::create(rewriter, loc, type, stored)
        .getResult(0);
  }

  /// The number of entities in `archetype`, as an index, loaded at the
  /// insertion point: spawns and despawns change it, so it is not cached.
  Value count(Location loc, const WorldArchetype &archetype) {
    Value position =
        arith::ConstantIndexOp::create(rewriter, loc, archetype.index);
    Value count = memref::LoadOp::create(rewriter, loc, getCounts(),
                                         ValueRange{position});
    return arith::IndexCastOp::create(rewriter, loc, rewriter.getIndexType(),
                                      count);
  }

  /// Store `count` (an index) as the number of entities in `archetype`.
  void setCount(Location loc, const WorldArchetype &archetype, Value count) {
    Value position =
        arith::ConstantIndexOp::create(rewriter, loc, archetype.index);
    Value value = arith::IndexCastOp::create(
        rewriter, loc, rewriter.getI64Type(), count);
    memref::StoreOp::create(rewriter, loc, value, getCounts(),
                            ValueRange{position});
  }

  /// The pending list of rows to despawn from `archetype`, and a
  /// one-element view of their number.
  Value pendingList(const WorldArchetype &archetype) {
    return view(archetype.pendingOffset, archetype.capacity,
                rewriter.getI32Type());
  }
  Value pendingCount(const WorldArchetype &archetype) {
    return view(archetype.pendingCountOffset, 1, rewriter.getI64Type());
  }
  Value pendingActions(const WorldArchetype &archetype) {
    return view(archetype.pendingActionOffset, archetype.capacity,
                rewriter.getI32Type());
  }
  Value moveValues(const WorldArchetype &archetype,
                   const WorldColumn &column) {
    return view(column.offset, archetype.capacity,
                storageType(column.type));
  }
  /// The buffer `apply` fills from the entities of `archetype`: a target
  /// id and a value per row.
  std::pair<Value, Value> applyBuffer(Operation *apply,
                                      const WorldArchetype &archetype) {
    auto index =
        apply->getAttrOfType<IntegerAttr>(WorldLayout::kApplyIndexAttr);
    const WorldApply &entry = layout.applies[index.getInt()];
    const WorldApplyBuffer &buffer = entry.find(archetype.index);
    return {view(buffer.idOffset, archetype.capacity,
                 rewriter.getIntegerType(layout.entities.idBits)),
            view(buffer.valueOffset, archetype.capacity,
                 storageType(entry.type))};
  }
  /// Whether the program has reactive queries, so that some events are
  /// stamped.
  bool hasStamps() const { return layout.tickOffset != 0; }
  /// The tick that events happening now are stamped with: one past the
  /// counter, which reactive queries advance when they start.
  Value currentTick(Location loc) {
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value counter = memref::LoadOp::create(
        rewriter, loc, scalar(layout.tickOffset), ValueRange{zero});
    return arith::AddIOp::create(
        rewriter, loc, counter,
        arith::ConstantIntOp::create(rewriter, loc, 1, 64));
  }
  /// The tick counter, and the tick at which `query` (reactive) last
  /// started, as one-element i64 views.
  Value tickCounter() { return scalar(layout.tickOffset); }
  Value lastTick(QueryOp query) {
    auto index =
        query->getAttrOfType<IntegerAttr>(WorldLayout::kReactiveIndexAttr);
    return scalar(layout.reactiveOffsets[index.getInt()]);
  }
  /// A stamp column of `archetype`.
  Value stamps(const WorldArchetype &archetype, const WorldColumn &column) {
    return view(column.offset, archetype.capacity, rewriter.getI64Type());
  }
  /// An event log's per-segment counts: segment s's count of entries ever
  /// appended at index s * stride, its slowest reader's position after it.
  Value logCounts(const WorldLog &log) {
    return view(log.countsOffset,
                log.segments * WorldLog::kSegmentStride / 8,
                rewriter.getI64Type());
  }
  /// Where each segment of an event log ended when its reader started.
  Value logEnds(const WorldLog &log) {
    return view(log.endsOffset, log.segments, rewriter.getI64Type());
  }
  /// A reader's positions in an event log, one per segment.
  Value logPositions(const WorldLog &log, uint64_t offset) {
    return view(offset, log.segments, rewriter.getI64Type());
  }
  /// The ring of an event log: entity ids and ticks.
  Value logIds(const WorldLog &log) {
    return view(log.idsOffset, log.capacity,
                rewriter.getIntegerType(layout.entities.idBits));
  }
  Value logTicks(const WorldLog &log) {
    return view(log.ticksOffset, log.capacity, rewriter.getI64Type());
  }
  /// The id of the entity at `row` of `archetype`, in its stored form.
  Value entityId(Location loc, const WorldArchetype &archetype, Value row) {
    if (hasIds())
      return memref::LoadOp::create(rewriter, loc, ids(archetype),
                                    ValueRange{row});
    return packRows(loc, archetype, row);
  }

  /// The all-ones id, which is never alive: "no target" in apply buffers.
  Value noEntity(Location loc) {
    return arith::ConstantIntOp::create(rewriter, loc, -1,
                                        layout.entities.idBits);
  }

  /// The id of the entity in each row of `archetype`.
  Value ids(const WorldArchetype &archetype) {
    return view(archetype.idOffset, archetype.capacity,
                rewriter.getIntegerType(layout.entities.idBits));
  }

  /// Allocate an id for a new entity at `row` (an index) of `archetype`,
  /// the same way the generated C header does: pop the free list (the last
  /// freed slot), or take the next never-used slot with generation 0.
  Value allocateEntity(Location loc, const WorldArchetype &archetype,
                       Value row) {
    const EntityScheme &scheme = layout.entities;
    Type index = rewriter.getIndexType();
    // Rows: the id is the location itself.
    if (scheme.kind == EntityScheme::Rows)
      return packRows(loc, archetype, row);
    // Slots: slots are never freed, so take the next one; no generation.
    if (scheme.kind == EntityScheme::Slots) {
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value nextSlot = scalar(layout.nextSlotOffset);
      Value fresh = memref::LoadOp::create(rewriter, loc, nextSlot,
                                           ValueRange{zero});
      memref::StoreOp::create(
          rewriter, loc,
          arith::AddIOp::create(
              rewriter, loc, fresh,
              arith::ConstantIntOp::create(rewriter, loc, 1, 64)),
          nextSlot, ValueRange{zero});
      Value slot = toIndex(loc, fresh);
      setLocation(loc, slot, archetype, row);
      return arith::IndexCastUIOp::create(
          rewriter, loc, rewriter.getIntegerType(scheme.idBits), slot);
    }
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one64 = arith::ConstantIntOp::create(rewriter, loc, 1, 64);
    Value freeHead = scalar(layout.freeHeadOffset);
    Value head = memref::LoadOp::create(rewriter, loc, freeHead,
                                        ValueRange{zero});
    Value reuse = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::ne, head,
        arith::ConstantIntOp::create(rewriter, loc, 0, 64));
    auto choose = scf::IfOp::create(rewriter, loc, TypeRange{index}, reuse,
                                    /*withElseRegion=*/true);
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(choose.thenBlock());
      // The head is the slot plus one; the slot's location holds the next.
      Value slot = toIndex(loc, arith::SubIOp::create(rewriter, loc, head,
                                                      one64));
      Value next = memref::LoadOp::create(rewriter, loc, locations(),
                                          ValueRange{slot});
      memref::StoreOp::create(rewriter, loc, widen(loc, next), freeHead,
                              ValueRange{zero});
      scf::YieldOp::create(rewriter, loc, slot);
      rewriter.setInsertionPointToStart(choose.elseBlock());
      Value nextSlot = scalar(layout.nextSlotOffset);
      Value fresh = memref::LoadOp::create(rewriter, loc, nextSlot,
                                           ValueRange{zero});
      memref::StoreOp::create(
          rewriter, loc, arith::AddIOp::create(rewriter, loc, fresh, one64),
          nextSlot, ValueRange{zero});
      Value freshIndex = toIndex(loc, fresh);
      memref::StoreOp::create(
          rewriter, loc,
          arith::ConstantIntOp::create(rewriter, loc, 0,
                                       scheme.generationStorageBits),
          generations(), ValueRange{freshIndex});
      scf::YieldOp::create(rewriter, loc, freshIndex);
    }
    Value slot = choose.getResult(0);
    setLocation(loc, slot, archetype, row);
    Value generation =
        memref::LoadOp::create(rewriter, loc, generations(), ValueRange{slot});
    return makeId(loc, generation, slot);
  }

  /// Free the id `entity`: bump its slot's generation, so the id is no
  /// longer alive, and push the slot on the free list through its location.
  void freeEntity(Location loc, Value entity) {
    const EntityScheme &scheme = layout.entities;
    Value slot = idSlot(loc, entity);
    Value generation =
        memref::LoadOp::create(rewriter, loc, generations(), ValueRange{slot});
    Value next = arith::AddIOp::create(
        rewriter, loc, generation,
        arith::ConstantIntOp::create(rewriter, loc, 1,
                                     scheme.generationStorageBits));
    // The generation wraps at its bit width, not its storage width.
    if (scheme.generationBits < scheme.generationStorageBits)
      next = arith::AndIOp::create(
          rewriter, loc, next,
          arith::ConstantIntOp::create(
              rewriter, loc, (int64_t(1) << scheme.generationBits) - 1,
              scheme.generationStorageBits));
    memref::StoreOp::create(rewriter, loc, next, generations(),
                            ValueRange{slot});
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value freeHead = scalar(layout.freeHeadOffset);
    Value head = memref::LoadOp::create(rewriter, loc, freeHead,
                                        ValueRange{zero});
    memref::StoreOp::create(rewriter, loc, narrow(loc, head), locations(),
                            ValueRange{slot});
    Value slot64 =
        arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(), slot);
    memref::StoreOp::create(
        rewriter, loc,
        arith::AddIOp::create(
            rewriter, loc, slot64,
            arith::ConstantIntOp::create(rewriter, loc, 1, 64)),
        freeHead, ValueRange{zero});
  }

  /// Record that the entity in slot `slot` (an index) lives at `row` (an
  /// index) of `archetype`.
  void setLocation(Location loc, Value slot, const WorldArchetype &archetype,
                   Value row) {
    const EntityScheme &scheme = layout.entities;
    Type type = rewriter.getIntegerType(scheme.locationBits);
    Value packed = arith::OrIOp::create(
        rewriter, loc,
        arith::ConstantIntOp::create(
            rewriter, loc, int64_t(archetype.index) << scheme.rowBits,
            scheme.locationBits),
        arith::IndexCastOp::create(rewriter, loc, type, row));
    memref::StoreOp::create(rewriter, loc, packed, locations(),
                            ValueRange{slot});
  }

  /// The archetype index (as a location-width integer) and row (an index)
  /// of the packed location of `slot`.
  std::pair<Value, Value> getLocation(Location loc, Value slot) {
    const EntityScheme &scheme = layout.entities;
    Value packed =
        memref::LoadOp::create(rewriter, loc, locations(), ValueRange{slot});
    Value archetype = arith::ShRUIOp::create(
        rewriter, loc, packed,
        arith::ConstantIntOp::create(rewriter, loc, scheme.rowBits,
                                     scheme.locationBits));
    Value row = arith::AndIOp::create(
        rewriter, loc, packed,
        arith::ConstantIntOp::create(
            rewriter, loc, (int64_t(1) << scheme.rowBits) - 1,
            scheme.locationBits));
    return {archetype, toIndex(loc, row)};
  }

  /// A Rows id: `archetype << rowBits | row`, at the id's width.
  Value packRows(Location loc, const WorldArchetype &archetype, Value row) {
    const EntityScheme &scheme = layout.entities;
    Type id = rewriter.getIntegerType(scheme.idBits);
    return arith::OrIOp::create(
        rewriter, loc,
        arith::ConstantIntOp::create(
            rewriter, loc, int64_t(archetype.index) << scheme.rowBits,
            scheme.idBits),
        arith::IndexCastUIOp::create(rewriter, loc, id, row));
  }

  /// The archetype index (at the id's width) and row (an index) of a Rows
  /// id.
  std::pair<Value, Value> unpackRows(Location loc, Value id) {
    const EntityScheme &scheme = layout.entities;
    Value archetype = arith::ShRUIOp::create(
        rewriter, loc, id,
        arith::ConstantIntOp::create(rewriter, loc, scheme.rowBits,
                                     scheme.idBits));
    Value row = arith::AndIOp::create(
        rewriter, loc, id,
        arith::ConstantIntOp::create(
            rewriter, loc, (int64_t(1) << scheme.rowBits) - 1, scheme.idBits));
    return {archetype, arith::IndexCastUIOp::create(
                           rewriter, loc, rewriter.getIndexType(), row)};
  }

  /// An id from a generation (stored width) and a slot (an index).
  Value makeId(Location loc, Value generation, Value slot) {
    const EntityScheme &scheme = layout.entities;
    Type id = rewriter.getIntegerType(scheme.idBits);
    Value high = arith::ShLIOp::create(
        rewriter, loc,
        scheme.generationStorageBits < scheme.idBits
            ? arith::ExtUIOp::create(rewriter, loc, id, generation).getResult()
            : generation,
        arith::ConstantIntOp::create(rewriter, loc, scheme.slotBits,
                                     scheme.idBits));
    Value low = arith::IndexCastUIOp::create(rewriter, loc, id, slot);
    return arith::OrIOp::create(rewriter, loc, high, low);
  }

  /// The slot (an index) of an id.
  Value idSlot(Location loc, Value id) {
    const EntityScheme &scheme = layout.entities;
    Value slot = arith::AndIOp::create(
        rewriter, loc, id,
        arith::ConstantIntOp::create(rewriter, loc,
                                     (int64_t(1) << scheme.slotBits) - 1,
                                     scheme.idBits));
    return arith::IndexCastUIOp::create(rewriter, loc,
                                        rewriter.getIndexType(), slot);
  }

  /// The generation of an id, at its stored width.
  Value idGeneration(Location loc, Value id) {
    const EntityScheme &scheme = layout.entities;
    Value generation = arith::ShRUIOp::create(
        rewriter, loc, id,
        arith::ConstantIntOp::create(rewriter, loc, scheme.slotBits,
                                     scheme.idBits));
    if (scheme.slotBits + scheme.generationBits < scheme.idBits)
      generation = arith::AndIOp::create(
          rewriter, loc, generation,
          arith::ConstantIntOp::create(
              rewriter, loc, (int64_t(1) << scheme.generationBits) - 1,
              scheme.idBits));
    Type type = rewriter.getIntegerType(scheme.generationStorageBits);
    if (scheme.generationStorageBits < scheme.idBits)
      generation = arith::TruncIOp::create(rewriter, loc, type, generation);
    return generation;
  }

  Value generations() {
    return view(layout.generationOffset, layout.entityCapacity,
                rewriter.getIntegerType(layout.entities.generationStorageBits));
  }
  Value locations() {
    return view(layout.locationOffset, layout.entityCapacity,
                rewriter.getIntegerType(layout.entities.locationBits));
  }

  /// A location-width value as an i64 counter, and back.
  Value widen(Location loc, Value value) {
    if (layout.entities.locationBits == 64)
      return value;
    return arith::ExtUIOp::create(rewriter, loc, rewriter.getI64Type(), value);
  }
  Value narrow(Location loc, Value value) {
    if (layout.entities.locationBits == 64)
      return value;
    return arith::TruncIOp::create(
        rewriter, loc, rewriter.getIntegerType(layout.entities.locationBits),
        value);
  }

  Value toIndex(Location loc, Value value) {
    return arith::IndexCastOp::create(rewriter, loc, rewriter.getIndexType(),
                                      value);
  }

  /// A statically shaped view of one column of `archetype`.
  Value column(const WorldArchetype &archetype, StringAttr component,
               StringAttr field) {
    const WorldColumn *column = archetype.find(component, field);
    Value &value = columns[{archetype.index, column->offset}];
    if (!value)
      value = atEntry([&](Location loc) {
        auto type = MemRefType::get({archetype.capacity},
                                    storageType(column->type));
        Value offset =
            arith::ConstantIndexOp::create(rewriter, loc, column->offset);
        return memref::ViewOp::create(rewriter, loc, type, arena, offset,
                                      ValueRange{});
      });
    return value;
  }

  /// A one-element view of a resource field.
  Value resourceField(StringAttr resource, StringAttr field) {
    const WorldResourceField *entry =
        layout.getResource(resource).find(field);
    Value &value = columns[{~0u, entry->offset}];
    if (!value)
      value = atEntry([&](Location loc) {
        auto type = MemRefType::get({1}, storageType(entry->type));
        Value offset =
            arith::ConstantIndexOp::create(rewriter, loc, entry->offset);
        return memref::ViewOp::create(rewriter, loc, type, arena, offset,
                                      ValueRange{});
      });
    return value;
  }

  /// A one-element i64 view (a counter in the header).
  Value scalar(uint64_t offset) {
    return view(offset, 1, rewriter.getI64Type());
  }
  /// An i32 array of the entity table.
  Value table(uint64_t offset) {
    return view(offset, layout.entityCapacity, rewriter.getI32Type());
  }

  /// The integer type ids are stored as.
  Type idType() { return rewriter.getIntegerType(layout.entities.idBits); }

  /// The key relations index the entity `id` (stored form) by, an index:
  /// its slot, or for row ids the id itself.
  Value entityKey(Location loc, Value id) {
    if (layout.entities.kind == EntityScheme::Rows)
      return arith::IndexCastUIOp::create(rewriter, loc,
                                          rewriter.getIndexType(), id);
    return idSlot(loc, id);
  }

  /// Whether `id` (stored form) can be a live entity: for row ids, a key in
  /// range (entities with row ids never die); for slot ids, a slot handed
  /// out and, with generations, the slot's current generation.
  Value isAlive(Location loc, Value id) {
    const EntityScheme &scheme = layout.entities;
    Value key = entityKey(loc, id);
    if (scheme.kind == EntityScheme::Rows)
      return arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::ult, key,
          arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys));
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value inUse = toIndex(loc, memref::LoadOp::create(
                                   rewriter, loc, scalar(layout.nextSlotOffset),
                                   ValueRange{zero}));
    Value handedOut = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::ult, key, inUse);
    if (scheme.kind == EntityScheme::Slots)
      return handedOut;
    auto current = scf::IfOp::create(rewriter, loc,
                                     TypeRange{rewriter.getI1Type()}, handedOut,
                                     /*withElseRegion=*/true);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(current.thenBlock());
    Value generation =
        memref::LoadOp::create(rewriter, loc, generations(), ValueRange{key});
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::CmpIOp::create(rewriter, loc,
                                         arith::CmpIPredicate::eq, generation,
                                         idGeneration(loc, id))});
    rewriter.setInsertionPointToStart(current.elseBlock());
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 1)});
    return current.getResult(0);
  }

  /// A relation's storage: source or target ids, a field, the dead flags,
  /// the offsets by source or target, the positions of edges by target, and
  /// the sorting cursors; its edge count and clean flag (i64 scalars).
  Value edgeIds(const WorldRelation &relation, bool source) {
    return view(source ? relation.sourceOffset : relation.targetOffset,
                relation.slots, idType());
  }
  Value edgeField(const WorldRelation &relation, const WorldColumn &field) {
    return view(field.offset, relation.slots, storageType(field.type));
  }
  Value edgeDead(const WorldRelation &relation) {
    return view(relation.deadOffset, relation.slots, rewriter.getI8Type());
  }
  /// What a linked tree's slot holds for its edge's source: the id and
  /// one, so that 0, which a new world's memory is, says there is none.
  Value slotOwner(Location loc, Value id) {
    return arith::AddIOp::create(
        rewriter, loc, id,
        arith::ConstantIntOp::create(rewriter, loc, 1,
                                     layout.entities.idBits));
  }
  Value ownerId(Location loc, Value owner) {
    return arith::SubIOp::create(
        rewriter, loc, owner,
        arith::ConstantIntOp::create(rewriter, loc, 1,
                                     layout.entities.idBits));
  }
  /// A linked tree's links, per entity key (a key and one, 0 for none),
  /// and where each key's entity is in the tree's list (and one).
  Value treeLinks(const WorldRelation &relation, uint64_t offset) {
    return view(offset, layout.entityKeys, offsetType(relation));
  }
  Value edgeOffsets(const WorldRelation &relation, bool in) {
    return view(relation.isSorted(in) ? relation.sortedOffset
                                      : relation.indexOffset,
                layout.entityKeys + 1, offsetType(relation));
  }
  Value indexEdges(const WorldRelation &relation) {
    return view(relation.indexEdgesOffset, relation.capacity,
                offsetType(relation));
  }
  Value edgeCursors(const WorldRelation &relation) {
    return view(relation.cursorOffset, layout.entityKeys,
                offsetType(relation));
  }
  Value edgeCount(const WorldRelation &relation) {
    return scalar(relation.countOffset);
  }
  Value edgesClean(const WorldRelation &relation) {
    return scalar(relation.cleanOffset);
  }
  Type offsetType(const WorldRelation &relation) {
    return rewriter.getIntegerType(relation.offsetBits);
  }
  /// A tree's entities with a parent, parents first, and how many.
  Value treeOrder(const WorldRelation &relation) {
    return view(relation.orderOffset, relation.orderCapacity, idType());
  }
  /// Per entity key, the number of the entity's connect in an ordered
  /// tree, and after those how many connects there were.
  Value connectNumbers(const WorldRelation &relation) {
    return view(relation.sequenceOffset, layout.entityKeys + 1,
                rewriter.getI64Type());
  }
  /// An order worked out for a query: the entities' ids, or their
  /// parents', and the two numbers per entity key it is worked out with.
  Value walkOrder(const WorldRelation &relation, bool parents) {
    return view(parents ? relation.walkParentsOffset
                        : relation.walkOrderOffset,
                relation.walkCapacity(), idType());
  }
  /// Which order is worked out (at 0) and how many it has (at 1).
  Value walkState(const WorldRelation &relation) {
    return view(relation.walkStateOffset, 2, rewriter.getI64Type());
  }
  Value walkPlaces(const WorldRelation &relation) {
    return view(relation.walkPlacesOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  Value walkNumbers(const WorldRelation &relation, bool cursors) {
    return view(cursors ? relation.walkCursorsOffset
                        : relation.walkSizesOffset,
                layout.entityKeys + 1, rewriter.getI64Type());
  }
  /// Per entity key, the tick at which the entity last got another
  /// sibling before it, and the sibling it had before the tree's order
  /// was last made.
  Value siblingTicks(const WorldRelation &relation) {
    return view(relation.siblingTicksOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  Value siblingsBefore(const WorldRelation &relation) {
    return view(relation.siblingsBeforeOffset, layout.entityKeys,
                offsetType(relation));
  }
  Value siblingsAfter(const WorldRelation &relation) {
    return view(relation.siblingsAfterOffset, layout.entityKeys,
                offsetType(relation));
  }
  /// Per entity key, the tick at which the entity last gained or lost a
  /// child in the tree.
  Value childTicks(const WorldRelation &relation) {
    return view(relation.childTicksOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  /// A sorted tree's: per entity key how many children the entity had
  /// when the rows were last put in order, and its siblings then.
  Value childCounts(const WorldRelation &relation) {
    return view(relation.childCountsOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  Value siblingIds(const WorldRelation &relation, bool after) {
    return view(after ? relation.siblingIdsAfterOffset
                      : relation.siblingIdsBeforeOffset,
                layout.entityKeys, idType());
  }
  /// Room to go down a tree without links in: entity keys.
  Value reachStack(const WorldRelation &relation) {
    return view(relation.reachStackOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  /// The entities that last got other children or siblings in the tree,
  /// for the queries that follow events: their ids and the ticks, in a
  /// ring, and how many there have been ever and the tick of the newest
  /// that the ring has lost (i64 each).
  Value touchedIds(const WorldRelation &relation) {
    return view(relation.touchedOffset, WorldRelation::kTouched, idType());
  }
  Value touchedTicks(const WorldRelation &relation) {
    return view(relation.touchedTicksOffset, WorldRelation::kTouched,
                rewriter.getI64Type());
  }
  Value touchedState(const WorldRelation &relation) {
    return view(relation.touchedStateOffset, 2, rewriter.getI64Type());
  }
  /// The entity `id` has other children or siblings from here on.
  void touch(Location loc, const WorldRelation &relation, Value id) {
    if (!relation.touchedOffset)
      return;
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    Value state = touchedState(relation);
    Value count = memref::LoadOp::create(rewriter, loc, state, ValueRange{zero});
    Value slot = arith::AndIOp::create(
        rewriter, loc, toIndex(loc, count),
        arith::ConstantIndexOp::create(rewriter, loc,
                                       WorldRelation::kTouched - 1));
    Value ticks = touchedTicks(relation);
    // (The entry this one takes the place of is lost; the first ones take
    // the place of none, whose tick is 0.)
    memref::StoreOp::create(
        rewriter, loc,
        memref::LoadOp::create(rewriter, loc, ticks, ValueRange{slot}), state,
        ValueRange{one});
    memref::StoreOp::create(rewriter, loc, id, touchedIds(relation),
                            ValueRange{slot});
    memref::StoreOp::create(rewriter, loc, currentTick(loc), ticks,
                            ValueRange{slot});
    memref::StoreOp::create(
        rewriter, loc,
        arith::AddIOp::create(rewriter, loc, count,
                              arith::ConstantIntOp::create(rewriter, loc, 1, 64)),
        state, ValueRange{zero});
  }
  /// Per entity key, what an ordered tree's children are ordered by, as
  /// read when the order was last made.
  Value orderKeys(const WorldRelation &relation) {
    return view(relation.orderKeysOffset, layout.entityKeys,
                rewriter.getI64Type());
  }
  /// Per entity key, the tick at which the entity was last connected, and
  /// after those the tick of the latest connect of any (at
  /// `latestConnect`).
  Value connectedTicks(const WorldRelation &relation) {
    return view(relation.connectedOffset, layout.entityKeys + 1,
                rewriter.getI64Type());
  }
  Value latestConnect(Location loc) {
    return arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys);
  }
  /// A linked tree's marks: a bit per element of its list.
  Value treeMarks(const WorldRelation &relation) {
    return view(relation.marksOffset, relation.markWords(),
                rewriter.getI64Type());
  }
  Value treeOrderParents(const WorldRelation &relation) {
    return view(relation.orderParentOffset, relation.orderCapacity, idType());
  }
  /// A sorted archetype: how many of its entities have no parent, and per
  /// row the parent's row.
  Value rootCount(const WorldArchetype &archetype) {
    return scalar(archetype.rootCountOffset);
  }
  Value parentRows(const WorldArchetype &archetype) {
    return view(archetype.parentRowOffset, archetype.capacity,
                rewriter.getI32Type());
  }
  /// A sorted tree in several archetypes: per row the rows of its
  /// children in the tree's archetype number `holder`, from `begin` to
  /// the other, and a mark per row.
  Value childRange(const WorldArchetype &archetype, unsigned holder,
                   bool begin) {
    auto [first, last] = archetype.childRangeOffsets[holder];
    return view(begin ? first : last, archetype.capacity,
                rewriter.getI32Type());
  }
  Value rowMarks(const WorldArchetype &archetype) {
    return view(archetype.marksOffset, archetype.markWords(),
                rewriter.getI64Type());
  }
  /// Per row of an archetype of an ordered tree sorted in several: the id
  /// of the sibling before.
  Value beforeIds(const WorldArchetype &archetype) {
    return view(archetype.beforeIdOffset, archetype.capacity, idType());
  }
  Value afterIds(const WorldArchetype &archetype) {
    return view(archetype.afterIdOffset, archetype.capacity, idType());
  }
  /// Per row of a sorted archetype, the tick of its entity's last connect.
  Value connectedRows(const WorldArchetype &archetype) {
    return view(archetype.connectedRowOffset, archetype.capacity,
                rewriter.getI64Type());
  }
  /// And the rows of its children, from `begin` to the other.
  Value childRows(const WorldArchetype &archetype, bool begin) {
    return view(begin ? archetype.childBeginOffset : archetype.childEndOffset,
                archetype.capacity, rewriter.getI32Type());
  }
  /// Where a sorted tree lives in several archetypes: per row its parent's
  /// packed location, per depth the archetype's first row of it, and the
  /// tree's depth.
  Value parentLocations(const WorldArchetype &archetype) {
    return view(archetype.parentLocationOffset, archetype.capacity,
                rewriter.getIntegerType(layout.entities.locationBits));
  }
  Value levelStarts(const WorldArchetype &archetype) {
    return view(archetype.levelStartOffset, archetype.levelCapacity,
                rewriter.getI32Type());
  }
  Value treeDepth(const WorldRelation &relation) {
    return scalar(relation.depthOffset);
  }
  /// The archetype (at a location's width) and the row (an index) of a
  /// packed location.
  std::pair<Value, Value> unpackLocation(Location loc, Value packed) {
    const EntityScheme &scheme = layout.entities;
    Value archetype = arith::ShRUIOp::create(
        rewriter, loc, packed,
        arith::ConstantIntOp::create(rewriter, loc, scheme.rowBits,
                                     scheme.locationBits));
    Value row = arith::AndIOp::create(
        rewriter, loc, packed,
        arith::ConstantIntOp::create(
            rewriter, loc, (int64_t(1) << scheme.rowBits) - 1,
            scheme.locationBits));
    return {archetype, toIndex(loc, row)};
  }
  Value treeOrderCount(const WorldRelation &relation) {
    return scalar(relation.orderCountOffset);
  }
  Value sortedCount(const WorldRelation &relation) {
    return scalar(relation.sortedCountOffset);
  }
  /// A linked tree's list, where ids are not rows: each entity's packed
  /// location and its parent's, and whether they are stale.
  Value treeOrderLocations(const WorldRelation &relation, bool parent) {
    return view(parent ? relation.orderParentLocationOffset
                       : relation.orderLocationOffset,
                relation.orderCapacity,
                rewriter.getIntegerType(layout.entities.locationBits));
  }
  /// A tree sorted in several archetypes: per element of the list where
  /// the entity's row is, or its parent's.
  Value rowOrder(const WorldRelation &relation, bool parent) {
    return view(parent ? relation.rowOrderParentOffset
                       : relation.rowOrderOffset,
                relation.orderCapacity,
                rewriter.getIntegerType(layout.entities.locationBits));
  }
  Value treeStale(const WorldRelation &relation) {
    return scalar(relation.staleOffset);
  }
  /// The packed location of the entity `id` (in its stored form), from
  /// the entity table.
  Value packedLocation(Location loc, Value id) {
    return memref::LoadOp::create(rewriter, loc, locations(),
                                  ValueRange{idSlot(loc, id)});
  }
  /// A view of `elements` values of `type` at `offset`.
  Value array(uint64_t offset, int64_t elements, Type type) {
    return view(offset, elements, type);
  }

  /// The per-edge buffers of an apply inside `ent.edges`: target ids and
  /// values, by position in the loop's order.
  std::pair<Value, Value> edgeApplyBuffer(Operation *apply) {
    auto index =
        apply->getAttrOfType<IntegerAttr>(WorldLayout::kApplyIndexAttr);
    const WorldApply &entry = layout.applies[index.getInt()];
    return {view(entry.edgeIdOffset, entry.edgeCapacity, idType()),
            view(entry.edgeValueOffset, entry.edgeCapacity,
                 storageType(entry.type))};
  }

  /// The buffers of a connect inside a query, for the rows of `archetype`:
  /// source ids, target ids, and a column per field.
  struct ConnectBuffers {
    Value sources, targets;
    SmallVector<Value> values;
  };
  ConnectBuffers connectBuffer(ConnectOp connect,
                               const WorldArchetype &archetype) {
    auto index =
        connect->getAttrOfType<IntegerAttr>(WorldLayout::kConnectIndexAttr);
    const WorldConnect::Buffer &buffer =
        layout.connects[index.getInt()].find(archetype.index);
    const WorldRelation &relation =
        layout.getRelation(connect.getRelationAttr().getAttr());
    ConnectBuffers result;
    result.sources = view(buffer.sourceOffset, archetype.capacity, idType());
    result.targets = view(buffer.targetOffset, archetype.capacity, idType());
    for (auto [offset, field] :
         llvm::zip(buffer.valueOffsets, relation.fields))
      result.values.push_back(
          view(offset, archetype.capacity, storageType(field.type)));
    return result;
  }

private:

  Value getCounts() {
    if (!countsView)
      countsView = atEntry([&](Location loc) {
        auto countsType = MemRefType::get(
            {static_cast<int64_t>(layout.archetypes.size())},
            IntegerType::get(rewriter.getContext(), 64));
        Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
        return memref::ViewOp::create(rewriter, loc, countsType, arena, zero,
                                      ValueRange{});
      });
    return countsView;
  }

  /// A view of `size` elements of `type` at `offset`, created once.
  Value view(uint64_t offset, int64_t size, Type type) {
    // (By what it is a view of, too: in a program without entities the
    // arrays with a place per entity are empty, and several start at the
    // same offset.)
    Value &value = views[{offset, MemRefType::get({size}, type)}];
    if (!value)
      value = atEntry([&](Location loc) {
        Value start = arith::ConstantIndexOp::create(rewriter, loc, offset);
        return memref::ViewOp::create(rewriter, loc,
                                      MemRefType::get({size}, type), arena,
                                      start, ValueRange{});
      });
    return value;
  }

  template <typename Build>
  Value atEntry(Build build) {
    OpBuilder::InsertionGuard guard(rewriter);
    if (lastCreated)
      rewriter.setInsertionPointAfter(lastCreated);
    else
      rewriter.setInsertionPointToStart(arena.getParentBlock());
    Value value = build(arena.getLoc());
    lastCreated = value.getDefiningOp();
    return value;
  }

  IRRewriter &rewriter;
  const WorldLayout &layout;
  Value arena;
  Operation *lastCreated = nullptr;
  Value countsView;
  llvm::DenseMap<std::pair<unsigned, uint64_t>, Value> columns;
  llvm::DenseMap<std::pair<uint64_t, Type>, Value> views;
};

/// How entity loops are emitted.
struct LoopOptions {
  bool parallelEntities;
  int64_t parallelMinEntities;
  /// Pending log entries from which a reactive query walks its event logs'
  /// segments in parallel.
  int64_t parallelMinEvents;
  /// Entities of one depth of a sorted tree from which a cascading query
  /// visits the depth in parallel.
  int64_t parallelMinLevel;
  /// Emit remarks explaining lowering decisions.
  bool explain;
  /// Combine unobserved applies directly in loops that never run in
  /// parallel.
  bool directApplies;
};

} // namespace

/// Replace `op` (a system or schedule) by a function whose entry block is
/// `op`'s body, extended by the world arena. Returns the function and the
/// arena argument.
static std::pair<func::FuncOp, Value>
convertToFunc(IRRewriter &rewriter, Operation *op, StringRef name,
              Region &body, MemRefType arenaType) {
  Block &entry = body.front();
  SmallVector<Type> inputs(entry.getArgumentTypes());
  inputs.push_back(arenaType);

  rewriter.setInsertionPoint(op);
  auto func = func::FuncOp::create(rewriter, op->getLoc(), name,
                                   rewriter.getFunctionType(inputs, {}));
  rewriter.inlineRegionBefore(body, func.getBody(), func.getBody().end());
  Value arena = entry.addArgument(arenaType, op->getLoc());

  Operation *terminator = entry.getTerminator();
  rewriter.setInsertionPoint(terminator);
  rewriter.replaceOpWithNewOp<func::ReturnOp>(terminator);
  rewriter.eraseOp(op);
  return {func, arena};
}

static bool matches(const WorldArchetype &archetype, QueryOp query) {
  return matches(query, archetype.op);
}

/// True if the query changes which entities `archetype` holds: it spawns,
/// despawns, or adds or removes a component in a way that moves entities
/// of this archetype.
static bool isStructuralFor(QueryOp query, ArchetypeOp archetype) {
  WalkResult result = query.getBody().walk([&](Operation *op) {
    if (isa<SpawnOp, DespawnOp>(op))
      return WalkResult::interrupt();
    if (auto add = dyn_cast<AddOp>(op))
      if (classifyChange(archetype, add.getComponentAttr(), true)
              .isStructural())
        return WalkResult::interrupt();
    if (auto remove = dyn_cast<RemoveOp>(op))
      if (classifyChange(archetype, remove.getComponentAttr(), false)
              .isStructural())
        return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return result.wasInterrupted();
}

/// True if the query body only computes and accesses its own entity's
/// components, so its iterations are independent of each other.
static bool isEntityLocal(QueryOp query) {
  for (ArchetypeOp archetype : getMatchedArchetypes(query))
    if (isStructuralFor(query, archetype))
      return false;
  WalkResult result = query.getBody().walk([](Operation *op) {
    // Resource reads are fine: no query writes a resource. Adding and
    // removing components without moving (checked above) only writes the
    // entity's own row; its id is its own.
    // Lookups read other entities, but the verifier ensures the query does
    // not change what they read.
    // Applies write the entity's own slot of their buffer.
    // Edge loops visit the entity's own edges (each edge belongs to one
    // entity's range); connects fill the entity's own slot of a buffer.
    if (isa<GetOp, SetOp, ReadOp, AddOp, RemoveOp, EntityOp, HasOp, LookupOp,
            ApplyOp, AccumulateOp, YieldOp, EdgesOp, ConnectOp, DisconnectOp>(
            op) ||
        !hasOwnEffects(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !result.wasInterrupted();
}

/// Emit the loop over the entities of `archetype` at the insertion point
/// and call `emitBody` with the entity index, positioned inside the loop,
/// the number of entities the loop runs over, and whether it runs its
/// iterations in parallel.
/// Returns the outermost op emitted.
///
/// An archetype of capacity 1 holds at most one entity, so it gets a guard
/// instead of a loop. Iterations of entity-local bodies are independent,
/// so the loop may be an `scf.parallel`, but a parallel loop only pays for
/// its fork beyond some number of entities. An archetype whose capacity is
/// below that threshold never gets a parallel loop; otherwise the count is
/// checked at run time and the body is emitted twice, once per loop kind.
static Operation *
emitEntityLoops(IRRewriter &rewriter, Location loc,
                const WorldArchetype &archetype, WorldAccess &world,
                const LoopOptions &options, bool entityLocal,
                function_ref<void(Value entity, Value rows, bool parallel)>
                    emitBody,
                Value rows = Value()) {
  // The entities to visit: `rows` if given (counted when the query
  // started), or as many as the archetype holds now.
  Value count = rows ? rows : world.count(loc, archetype);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);

  if (archetype.capacity == 1) {
    Value present = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::sgt, count, zero);
    auto guard = scf::IfOp::create(rewriter, loc, present);
    rewriter.setInsertionPointToStart(guard.thenBlock());
    emitBody(zero, count, /*parallel=*/false);
    return guard;
  }

  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  auto emitSequential = [&]() -> Operation * {
    auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVar(), count, /*parallel=*/false);
    return loop;
  };
  auto emitParallel = [&]() -> Operation * {
    auto loop = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                        ValueRange{count}, ValueRange{one});
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVars().front(), count, /*parallel=*/true);
    return loop;
  };

  if (!options.parallelEntities || !entityLocal ||
      archetype.capacity < options.parallelMinEntities)
    return emitSequential();
  if (options.parallelMinEntities <= 1)
    return emitParallel();

  Value threshold = arith::ConstantIndexOp::create(
      rewriter, loc, options.parallelMinEntities);
  Value large = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::sge,
                                      count, threshold);
  auto branch = scf::IfOp::create(rewriter, loc, large,
                                  /*withElseRegion=*/true);
  rewriter.setInsertionPointToStart(branch.thenBlock());
  emitParallel();
  rewriter.setInsertionPointToStart(branch.elseBlock());
  emitSequential();
  return branch;
}

/// Load every resource read inside `loops` once, right before them. Sound
/// because no query writes a resource, and a fused sequence contains no
/// system that writes one: the value cannot change while the loops run.
static void hoistResourceReads(IRRewriter &rewriter, Operation *loops,
                               WorldAccess &world) {
  SmallVector<ReadOp> reads;
  loops->walk([&](ReadOp read) { reads.push_back(read); });
  for (ReadOp read : reads) {
    Value field = world.resourceField(read.getResourceAttr().getAttr(),
                                      read.getFieldAttr());
    rewriter.setInsertionPoint(loops);
    Value zero = arith::ConstantIndexOp::create(rewriter, read.getLoc(), 0);
    Value value = memref::LoadOp::create(rewriter, read.getLoc(), field,
                                         ValueRange{zero});
    rewriter.replaceOp(read, world.fromStorage(read.getLoc(), value,
                                               read.getType()));
  }
}

/// Lower the resource reads and writes left in `func` (those at system
/// level) to loads and stores in place.
static void lowerResourceAccesses(IRRewriter &rewriter, func::FuncOp func,
                                  WorldAccess &world) {
  SmallVector<Operation *> accesses;
  func.walk([&](Operation *op) {
    if (isa<ReadOp, WriteOp>(op))
      accesses.push_back(op);
  });
  for (Operation *op : accesses) {
    rewriter.setInsertionPoint(op);
    Value zero = arith::ConstantIndexOp::create(rewriter, op->getLoc(), 0);
    if (auto read = dyn_cast<ReadOp>(op)) {
      Value field = world.resourceField(read.getResourceAttr().getAttr(),
                                        read.getFieldAttr());
      Value value =
          memref::LoadOp::create(rewriter, op->getLoc(), field, ValueRange{zero});
      rewriter.replaceOp(read, world.fromStorage(op->getLoc(), value,
                                                 read.getType()));
    } else {
      auto write = cast<WriteOp>(op);
      Value field = world.resourceField(write.getResourceAttr().getAttr(),
                                        write.getFieldAttr());
      rewriter.replaceOpWithNewOp<memref::StoreOp>(
          write, world.toStorage(op->getLoc(), write.getValue()), field,
          ValueRange{zero});
    }
  }
}

/// True if every op in the query's body may also run for an entity that
/// lacks the query's optional components, whose fields hold stale or
/// uninitialised values: component and resource accesses (their slots
/// always exist), `scf.if` as structure, and pure, speculatable ops. Integer
/// division, for example, is not: it may be undefined on such values.
static bool canRunForAbsentEntities(QueryOp query) {
  WalkResult result = query.getBody().walk([](Operation *op) {
    if (isa<GetOp, SetOp, ReadOp, AddOp, RemoveOp, EntityOp, HasOp, LookupOp,
            ApplyOp, AccumulateOp, YieldOp, scf::IfOp, scf::YieldOp>(op))
      return WalkResult::advance();
    if (op->getNumRegions() == 0 && isPure(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !result.wasInterrupted();
}

/// List `entity` (a row of `archetype`) as pending with `action` (0 to
/// despawn, otherwise a move's code) and the values of a move that adds a
/// component. The rows come in ascending order; if the last listed row is
/// this one, the new action replaces it: the last structural change to an
/// entity in a query wins.
static void recordPending(IRRewriter &rewriter, Location loc,
                          const WorldArchetype &archetype, WorldAccess &world,
                          Value entity, unsigned action,
                          const WorldMove *move, ValueRange values) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value counter = world.pendingCount(archetype);
  Value count = memref::LoadOp::create(rewriter, loc, counter, ValueRange{zero});
  Value one64 = arith::ConstantIntOp::create(rewriter, loc, 1, 64);
  Value row = arith::IndexCastOp::create(rewriter, loc, rewriter.getI32Type(),
                                         entity);
  Value list = world.pendingList(archetype);
  // Is the last listed row this entity? (Only look if there is one.)
  Value any = arith::CmpIOp::create(
      rewriter, loc, arith::CmpIPredicate::sgt, count,
      arith::ConstantIntOp::create(rewriter, loc, 0, 64));
  auto check = scf::IfOp::create(rewriter, loc, TypeRange{rewriter.getI1Type()},
                                 any, /*withElseRegion=*/true);
  {
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(check.thenBlock());
    Value last = memref::LoadOp::create(
        rewriter, loc, list,
        ValueRange{world.toIndex(
            loc, arith::SubIOp::create(rewriter, loc, count, one64))});
    scf::YieldOp::create(rewriter, loc,
                         ValueRange{arith::CmpIOp::create(
                             rewriter, loc, arith::CmpIPredicate::eq, last,
                             row)});
    rewriter.setInsertionPointToStart(check.elseBlock());
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 1)});
  }
  Value same = check.getResult(0);
  Value slot64 = arith::SelectOp::create(
      rewriter, loc, same, arith::SubIOp::create(rewriter, loc, count, one64),
      count);
  Value slot = world.toIndex(loc, slot64);
  memref::StoreOp::create(rewriter, loc, row, list, ValueRange{slot});
  if (archetype.pendingActionOffset)
    memref::StoreOp::create(
        rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, action, 32),
        world.pendingActions(archetype), ValueRange{slot});
  if (move)
    for (auto [value, column] : llvm::zip(values, move->values))
      memref::StoreOp::create(rewriter, loc, world.toStorage(loc, value),
                              world.moveValues(archetype, column),
                              ValueRange{slot});
  memref::StoreOp::create(
      rewriter, loc,
      arith::SelectOp::create(rewriter, loc, same, count,
                              arith::AddIOp::create(rewriter, loc, count, one64)),
      counter, ValueRange{zero});
}

/// The segment of `log` that an event at `row` (an index) of an archetype
/// holding `rows` entities (an index) goes to, as an i64: row * segments /
/// rows, so that the contiguous row ranges of a parallel loop's threads map
/// to different segments. Without `rows`, the row's low bits.
static Value segmentOf(IRRewriter &rewriter, Location loc, WorldAccess &world,
                       const WorldLog &log, Value row, Value rows) {
  Type i64 = rewriter.getI64Type();
  Value at = arith::IndexCastOp::create(rewriter, loc, i64, row);
  if (log.segments == 1)
    return arith::ConstantIntOp::create(rewriter, loc, 0, 64);
  if (!rows)
    return arith::AndIOp::create(
        rewriter, loc, at,
        arith::ConstantIntOp::create(rewriter, loc, log.segments - 1, 64));
  Value scaled = arith::MulIOp::create(
      rewriter, loc, at,
      arith::ConstantIntOp::create(rewriter, loc, log.segments, 64));
  return arith::DivUIOp::create(
      rewriter, loc, scaled,
      arith::IndexCastOp::create(rewriter, loc, i64, rows));
}

/// Append the entity `id` (stored form) to `segment` (an i64) of `log` as
/// having had an event at `tick`, if `old` (its stamp before this event;
/// null for a new entity or a move, which are events by themselves) is not
/// `tick` already and `when` holds (null for always). Entries a reader has
/// not read are never written over: once the segment is full for its
/// slowest reader, an event is not appended but lost, to every reader
/// (one that has read further would otherwise not notice). So the event
/// moves the segment's count on, past what the slowest reader can hold,
/// and notes the new count as the segment's third number: a reader whose
/// position is before that has lost an event and scans (see openLogs),
/// after which its position is there or beyond. That is done for the
/// first event lost since a reader last finished with the log, not for
/// each: all who have not read on know already. With `atomic`, iterations of a
/// parallel loop may append concurrently: the count is advanced atomically
/// (rarely contended, since threads mostly own their segments) and never
/// moved back.
static void appendToLog(IRRewriter &rewriter, Location loc, WorldAccess &world,
                        const WorldLog &log, Value segment, Value id,
                        Value tick, Value old, Value when, bool atomic) {
  auto i64 = [&](int64_t value) {
    return arith::ConstantIntOp::create(rewriter, loc, value, 64);
  };
  Value event = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
  if (old)
    event = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne, old,
                                  tick);
  if (when)
    event = arith::AndIOp::create(rewriter, loc, event, when);
  Value counts = world.logCounts(log);
  Value base = arith::MulIOp::create(rewriter, loc, segment,
                                     i64(WorldLog::kSegmentStride / 8));
  Value countAt = world.toIndex(loc, base);
  Value slowestAt =
      world.toIndex(loc, arith::AddIOp::create(rewriter, loc, base, i64(1)));
  // A segment whose readers have been told of a lost event, and none of
  // which has finished with the log since (its fourth number, which a
  // reader takes back, see closeLogs), is as full as it was: the event is
  // lost as well, and that is all there is to do. So that is looked at
  // first, and the events of a query that changes more than its logs
  // hold cost a look each.
  Value toldAt =
      world.toIndex(loc, arith::AddIOp::create(rewriter, loc, base, i64(3)));
  Value untold = arith::CmpIOp::create(
      rewriter, loc, arith::CmpIPredicate::eq,
      memref::LoadOp::create(rewriter, loc, counts, ValueRange{toldAt}),
      i64(0));
  OpBuilder::InsertionGuard guard(rewriter);
  auto live = scf::IfOp::create(
      rewriter, loc, arith::AndIOp::create(rewriter, loc, event, untold));
  rewriter.setInsertionPointToStart(live.thenBlock());
  Value count =
      memref::LoadOp::create(rewriter, loc, counts, ValueRange{countAt});
  Value slowest =
      memref::LoadOp::create(rewriter, loc, counts, ValueRange{slowestAt});
  Value pending = arith::SubIOp::create(rewriter, loc, count, slowest);
  Value room = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::slt,
                                     pending, i64(log.segmentCapacity));
  auto append = scf::IfOp::create(rewriter, loc, room,
                                  /*withElseRegion=*/true);
  rewriter.setInsertionPointToStart(append.thenBlock());
  Value position;
  if (atomic) {
    position = memref::AtomicRMWOp::create(rewriter, loc,
                                           arith::AtomicRMWKind::addi, i64(1),
                                           counts, ValueRange{countAt});
  } else {
    position = count;
    memref::StoreOp::create(rewriter, loc,
                            arith::AddIOp::create(rewriter, loc, count, i64(1)),
                            counts, ValueRange{countAt});
  }
  Value slot = world.toIndex(
      loc, arith::AddIOp::create(
               rewriter, loc,
               arith::MulIOp::create(rewriter, loc, segment,
                                     i64(log.segmentCapacity)),
               arith::AndIOp::create(rewriter, loc, position,
                                     i64(log.segmentCapacity - 1))));
  memref::StoreOp::create(rewriter, loc, id, world.logIds(log),
                          ValueRange{slot});
  memref::StoreOp::create(rewriter, loc, tick, world.logTicks(log),
                          ValueRange{slot});

  // Lost: told once.
  rewriter.setInsertionPointToStart(append.elseBlock());
  memref::StoreOp::create(rewriter, loc, i64(1), counts, ValueRange{toldAt});
  Value lostAt =
      world.toIndex(loc, arith::AddIOp::create(rewriter, loc, base, i64(2)));
  Value atLeast = arith::AddIOp::create(rewriter, loc, slowest,
                                        i64(log.segmentCapacity));
  if (atomic) {
    memref::AtomicRMWOp::create(rewriter, loc, arith::AtomicRMWKind::maxs,
                                atLeast, counts, ValueRange{countAt});
    Value before = memref::AtomicRMWOp::create(
        rewriter, loc, arith::AtomicRMWKind::addi, i64(1), counts,
        ValueRange{countAt});
    memref::AtomicRMWOp::create(
        rewriter, loc, arith::AtomicRMWKind::maxs,
        arith::AddIOp::create(rewriter, loc, before, i64(1)), counts,
        ValueRange{lostAt});
  } else {
    Value past = arith::AddIOp::create(
        rewriter, loc, arith::MaxSIOp::create(rewriter, loc, count, atLeast),
        i64(1));
    memref::StoreOp::create(rewriter, loc, past, counts, ValueRange{countAt});
    memref::StoreOp::create(rewriter, loc, past, counts, ValueRange{lostAt});
  }
}

/// The stamp columns of `archetype` that an event updates: `kind` of
/// `component`, and for Changed, a write to `field` (every field if null).
static SmallVector<const WorldColumn *>
stampsFor(const WorldArchetype &archetype, Trigger::Kind kind,
          StringAttr component, StringAttr field = StringAttr()) {
  SmallVector<const WorldColumn *> columns;
  for (const WorldColumn &column : archetype.columns) {
    if (!column.isStamp() || column.stamp->kind != kind ||
        column.stamp->component != component)
      continue;
    if (kind == Trigger::Changed && field &&
        !column.stamp->field.getValue().empty() &&
        column.stamp->field != field)
      continue;
    columns.push_back(&column);
  }
  return columns;
}

static void combineDirectly(IRRewriter &rewriter, ApplyOp apply,
                            const WorldLayout &layout, WorldAccess &world,
                            Value id, Value value, Value tick);
static Value combine(IRRewriter &rewriter, Location loc, StringRef rule,
                     Value a, Value b);

/// Marks an `ent.apply` whose field nothing else in its query touches (no
/// get, set or lookup of the field, no other apply to it, no add or remove
/// of the component), or an `ent.accumulate` whose resource field nothing
/// else in its query reads or accumulates into. Combining its values as the
/// query visits the entities, one after another, then gives the result
/// combining them at the query's end would (same order, nothing sees the
/// field in between), so a sequential loop does that instead of filling a
/// buffer.
static constexpr llvm::StringLiteral kUnobservedAttr = "ent.unobserved";

/// Marks an `ent.lookup` or `ent.apply` whose entity is the other end of
/// an edge loop's edge, of the component that end is trusted to have (see
/// getTrustedEndpoint), or the ancestor a ref up a tree was found to lead
/// to: it is located without checks.
static constexpr llvm::StringLiteral kTrustedAttr = "ent.trusted";

static bool appliesDirectly(Operation *apply, bool directApplies) {
  return directApplies && isa<ApplyOp, AccumulateOp>(apply) &&
         apply->hasAttr(kUnobservedAttr);
}

/// What recordPending does, for a query that does not visit rows in their
/// order (a cascading one), which applying the pending rows goes by: the
/// row is marked in its own place of the pending list, with its action
/// and a move's values in the same place of theirs, and the query's end
/// makes the list of the marked rows, in order. The last change to an
/// entity is the one that stays.
static void markPending(IRRewriter &rewriter, Location loc,
                        const WorldArchetype &archetype, WorldAccess &world,
                        Value row, unsigned action, const WorldMove *move,
                        ValueRange values) {
  memref::StoreOp::create(rewriter, loc,
                          arith::ConstantIntOp::create(rewriter, loc, 1, 32),
                          world.pendingList(archetype), ValueRange{row});
  if (archetype.pendingActionOffset)
    memref::StoreOp::create(
        rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, action, 32),
        world.pendingActions(archetype), ValueRange{row});
  if (move)
    for (auto [value, column] : llvm::zip(values, move->values))
      memref::StoreOp::create(rewriter, loc, world.toStorage(loc, value),
                              world.moveValues(archetype, column),
                              ValueRange{row});
}

/// Whether a write of `value` to a field that holds `old` (both in their
/// stored form) changes it, at the insertion point: an event for a
/// reactive query is a field getting another value, not a write of the
/// one it has. Null where the two cannot be compared with one op (a
/// text): such a write counts.
static Value emitDiffers(IRRewriter &rewriter, Location loc, Value old,
                         Value value) {
  Type type = value.getType();
  if (type != old.getType())
    return Value();
  if (isa<IntegerType, IndexType>(type))
    return arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne, old,
                                 value);
  if (isa<FloatType>(type))
    return arith::CmpFOp::create(rewriter, loc, arith::CmpFPredicate::UNE, old,
                                 value);
  return Value();
}

/// Replace the get/set/add/remove/despawn/entity ops nested in `roots` by
/// loads and stores at `entity` in the columns of `archetype`. With a
/// `mask`, every store keeps the old value where the mask is false: the
/// body ran for an entity it does not apply to.
static void lowerAccesses(IRRewriter &rewriter, ArrayRef<Operation *> roots,
                          const WorldArchetype &archetype, WorldAccess &world,
                          const WorldLayout &layout, Value entity,
                          Value rows, Value mask, Value tick, bool parallel,
                          bool directApplies, bool marksPending = false) {
  auto column = [&](StringAttr component, StringAttr field) {
    return world.column(archetype, component, field);
  };
  auto store = [&](Location loc, Value value, Value memref) {
    value = world.toStorage(loc, value);
    if (mask) {
      Value old = memref::LoadOp::create(rewriter, loc, memref,
                                         ValueRange{entity});
      value = arith::SelectOp::create(rewriter, loc, mask, value, old);
    }
    memref::StoreOp::create(rewriter, loc, value, memref, ValueRange{entity});
  };
  auto setPresence = [&](Location loc, StringAttr component, int present) {
    Value presence = column(component, rewriter.getStringAttr(""));
    Value value = arith::ConstantIntOp::create(rewriter, loc, present, 8);
    store(loc, value, presence);
  };
  // Record an event for reactive queries: store the current tick in the
  // stamps it updates (none unless some query observes it).
  // An entity whose stamp moves on to this tick is appended to the
  // stamp's event log, if it has one.
  // (`when`: where the event happened at all, a field got another value;
  // null for always.)
  auto stamp = [&](Location loc, Trigger::Kind kind, StringAttr component,
                   StringAttr field = StringAttr(), Value when = Value()) {
    Value happened = mask && when
                         ? arith::AndIOp::create(rewriter, loc, mask, when)
                               .getResult()
                         : mask ? mask : when;
    for (const WorldColumn *column :
         stampsFor(archetype, kind, component, field)) {
      assert(tick && "an event is stamped without a tick");
      Value stamps = world.stamps(archetype, *column);
      const WorldLog *log = layout.findLog(*column->stamp);
      Value old = log || happened
                      ? memref::LoadOp::create(rewriter, loc, stamps,
                                               ValueRange{entity})
                            .getResult()
                      : Value();
      memref::StoreOp::create(
          rewriter, loc,
          happened ? arith::SelectOp::create(rewriter, loc, happened, tick,
                                             old)
                         .getResult()
                   : tick,
          stamps, ValueRange{entity});
      if (log)
        appendToLog(rewriter, loc, world, *log,
                    // (By row range only where threads share the
                    // log: it costs a division an event.)
                    segmentOf(rewriter, loc, world, *log, entity,
                              parallel ? rows : Value()),
                    world.entityId(loc, archetype, entity), tick, old,
                    happened, parallel);
    }
  };

  SmallVector<Operation *> accesses;
  for (Operation *root : roots)
    root->walk([&](Operation *op) {
      if (isa<GetOp, SetOp, AddOp, RemoveOp, DespawnOp, EntityOp, ApplyOp,
              AccumulateOp, ConnectOp>(op))
        accesses.push_back(op);
    });
  for (Operation *op : accesses) {
    rewriter.setInsertionPoint(op);
    Location loc = op->getLoc();
    if (auto get = dyn_cast<GetOp>(op)) {
      FlatSymbolRefAttr component =
          cast<RefType>(get.getRef().getType()).getComponent();
      Value value = memref::LoadOp::create(
          rewriter, loc, column(component.getAttr(), get.getFieldAttr()),
          ValueRange{entity});
      rewriter.replaceOp(get, world.fromStorage(loc, value, get.getType()));
    } else if (auto set = dyn_cast<SetOp>(op)) {
      FlatSymbolRefAttr component =
          cast<RefType>(set.getRef().getType()).getComponent();
      // An event, and another order for a tree ordered by the field,
      // only where the field gets another value: compared where either
      // matters.
      Value target = column(component.getAttr(), set.getFieldAttr());
      bool ordersTree =
          llvm::any_of(layout.relations, [&](const WorldRelation &relation) {
            return relation.orderComponent == component.getAttr() &&
                   relation.orderField == set.getFieldAttr();
          });
      Value differs;
      if (ordersTree || !stampsFor(archetype, Trigger::Changed,
                                   component.getAttr(), set.getFieldAttr())
                             .empty())
        differs = emitDiffers(
            rewriter, loc,
            memref::LoadOp::create(rewriter, loc, target, ValueRange{entity}),
            world.toStorage(loc, set.getValue()));
      store(loc, set.getValue(), target);
      stamp(loc, Trigger::Changed, component.getAttr(), set.getFieldAttr(),
            differs);
      Value moved = mask && differs
                        ? arith::AndIOp::create(rewriter, loc, mask, differs)
                              .getResult()
                        : mask ? mask : differs;
      // What a tree's children are ordered by: the tree is looked over
      // when the query ends (see noteOrderWrites).
      for (const WorldRelation &relation : layout.relations) {
        if (relation.orderComponent != component.getAttr() ||
            relation.orderField != set.getFieldAttr())
          continue;
        Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
        Value clean = world.edgesClean(relation);
        Value unclean = arith::ConstantIntOp::create(rewriter, loc, 0, 64);
        if (moved)
          unclean = arith::SelectOp::create(
              rewriter, loc, moved, unclean,
              memref::LoadOp::create(rewriter, loc, clean, ValueRange{zero}));
        memref::StoreOp::create(rewriter, loc, unclean, clean,
                                ValueRange{zero});
      }
      rewriter.eraseOp(set);
    } else if (isa<AddOp, RemoveOp>(op)) {
      bool isAdd = isa<AddOp>(op);
      auto componentRef = cast<FlatSymbolRefAttr>(op->getAttr("component"));
      StringAttr component = componentRef.getAttr();
      ValueRange values = isAdd ? cast<AddOp>(op).getValues() : ValueRange();
      auto componentOp =
          SymbolTable::lookupNearestSymbolFrom<ComponentOp>(op, componentRef);
      switch (classifyChange(archetype.op, componentRef, isAdd).kind) {
      case ComponentChange::Presence:
        if (isAdd)
          for (auto [value, field] :
               llvm::zip(values, componentOp.getFieldNames()))
            store(loc, value, column(component, cast<StringAttr>(field)));
        setPresence(loc, component, isAdd ? 1 : 0);
        stamp(loc, isAdd ? Trigger::Added : Trigger::Removed, component);
        if (isAdd)
          stamp(loc, Trigger::Changed, component);
        break;
      case ComponentChange::Overwrite:
        for (auto [value, field] :
             llvm::zip(values, componentOp.getFieldNames()))
          store(loc, value, column(component, cast<StringAttr>(field)));
        stamp(loc, Trigger::Changed, component);
        break;
      case ComponentChange::Move: {
        // Deferred like a despawn; a moving body never runs masked.
        assert(!mask && "move in a masked body");
        const WorldMove *move = archetype.findMove(component, isAdd);
        if (marksPending)
          markPending(rewriter, loc, archetype, world, entity, move->code,
                      move, values);
        else
          recordPending(rewriter, loc, archetype, world, entity, move->code,
                        move, values);
        break;
      }
      case ComponentChange::Nothing:
      case ComponentChange::NoTarget:
        break;
      }
      rewriter.eraseOp(op);
    } else if (auto apply = dyn_cast<ApplyOp>(op);
               apply && appliesDirectly(apply, directApplies)) {
      // Combine now: the loop visits entities in the order the query's end
      // would combine them, and nothing in the query sees the field.
      Value id = world.toStorage(loc, apply.getEntity());
      Value value = world.toStorage(loc, apply.getValue());
      if (mask) {
        auto ifApplies = scf::IfOp::create(rewriter, loc, mask);
        rewriter.setInsertionPointToStart(ifApplies.thenBlock());
      }
      combineDirectly(rewriter, apply, layout, world, id, value, tick);
      rewriter.eraseOp(op);
    } else if (auto apply = dyn_cast<ApplyOp>(op)) {
      // Fill this entity's slot of the buffer; the query's end combines.
      // An entity the masked body does not apply to sends nothing.
      auto [ids, values] = world.applyBuffer(apply, archetype);
      Value id = world.toStorage(loc, apply.getEntity());
      if (mask)
        id = arith::SelectOp::create(rewriter, loc, mask, id,
                                     world.noEntity(loc));
      memref::StoreOp::create(rewriter, loc, id, ids, ValueRange{entity});
      memref::StoreOp::create(rewriter, loc,
                              world.toStorage(loc, apply.getValue()), values,
                              ValueRange{entity});
      rewriter.eraseOp(op);
    } else if (auto accumulate = dyn_cast<AccumulateOp>(op);
               accumulate && appliesDirectly(accumulate, directApplies)) {
      // Combine into the resource now, in the order the query's end would.
      if (mask) {
        auto ifApplies = scf::IfOp::create(rewriter, loc, mask);
        rewriter.setInsertionPointToStart(ifApplies.thenBlock());
      }
      Value field = world.resourceField(accumulate.getResourceAttr().getAttr(),
                                        accumulate.getFieldAttr());
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value old = memref::LoadOp::create(rewriter, loc, field,
                                         ValueRange{zero});
      memref::StoreOp::create(
          rewriter, loc,
          combine(rewriter, loc, accumulate.getRule(), old,
                  world.toStorage(loc, accumulate.getValue())),
          field, ValueRange{zero});
      rewriter.eraseOp(op);
    } else if (auto accumulate = dyn_cast<AccumulateOp>(op)) {
      // The same, with "sent" (0) for a target id.
      auto [ids, values] = world.applyBuffer(accumulate, archetype);
      Value sent = arith::ConstantIntOp::create(
          rewriter, loc,
          cast<MemRefType>(ids.getType()).getElementType(), 0);
      if (mask)
        sent = arith::SelectOp::create(rewriter, loc, mask, sent,
                                       world.noEntity(loc));
      memref::StoreOp::create(rewriter, loc, sent, ids, ValueRange{entity});
      memref::StoreOp::create(rewriter, loc, accumulate.getValue(), values,
                              ValueRange{entity});
      rewriter.eraseOp(op);
    } else if (auto connect = dyn_cast<ConnectOp>(op)) {
      // Fill this entity's slot; the query's end appends the edge.
      WorldAccess::ConnectBuffers buffers =
          world.connectBuffer(connect, archetype);
      Value source = world.toStorage(loc, connect.getSource());
      if (mask)
        source = arith::SelectOp::create(rewriter, loc, mask, source,
                                         world.noEntity(loc));
      memref::StoreOp::create(rewriter, loc, source, buffers.sources,
                              ValueRange{entity});
      memref::StoreOp::create(rewriter, loc,
                              world.toStorage(loc, connect.getTarget()),
                              buffers.targets, ValueRange{entity});
      for (auto [value, column] :
           llvm::zip(connect.getValues(), buffers.values))
        memref::StoreOp::create(rewriter, loc, world.toStorage(loc, value),
                                column, ValueRange{entity});
      rewriter.eraseOp(op);
    } else if (auto entityOp = dyn_cast<EntityOp>(op)) {
      Value id = world.entityId(loc, archetype, entity);
      rewriter.replaceOp(op, world.fromStorage(loc, id, entityOp.getType()));
    } else {
      // Despawn is deferred: list the row; the query's end removes it. A
      // body with a despawn never runs masked (it is not speculatable).
      assert(!mask && "despawn in a masked body");
      if (marksPending)
        markPending(rewriter, loc, archetype, world, entity, /*action=*/0,
                    /*move=*/nullptr, {});
      else
        recordPending(rewriter, loc, archetype, world, entity, /*action=*/0,
                      /*move=*/nullptr, {});
      rewriter.eraseOp(op);
    }
  }
}

static Operation *lowerEdges(IRRewriter &rewriter, EdgesOp edges,
                             const WorldArchetype &archetype,
                             WorldAccess &world, const WorldLayout &layout,
                             Value row, Value tick, bool directApplies);
static SmallVector<Operation *> carryOwnFields(IRRewriter &rewriter,
                                               scf::ForOp loop);
namespace {
/// Where a ref `up` a relation leads for one entity: whether it has such
/// an ancestor, and the ancestor's id (in its stored form).
struct Ancestor {
  Value found, id;
  /// The ancestor is known to be alive and to have the component.
  bool trusted;
  /// Where it is, if that is known rather than its id: its row, in the
  /// one of `homes` that `where` (at a location's width) names; `where` is
  /// null where there is one home.
  SmallVector<const WorldArchetype *, 2> homes;
  Value where, row;
};
} // namespace
static Ancestor emitAncestor(IRRewriter &rewriter, Location loc,
                             const WorldLayout &layout, WorldAccess &world,
                             const WorldRelation &relation,
                             FlatSymbolRefAttr component, Value id,
                             Value parent = Value(), bool direct = false,
                             unsigned hops = 1);
static Ancestor emitSibling(IRRewriter &rewriter, Location loc,
                            const WorldLayout &layout, WorldAccess &world,
                            const WorldRelation &relation,
                            FlatSymbolRefAttr component,
                            const WorldArchetype &archetype, Value row,
                            bool after);
static std::pair<Value, Value> emitParent(IRRewriter &rewriter, Location loc,
                                          const WorldLayout &layout,
                                          WorldAccess &world,
                                          const WorldRelation &relation,
                                          Value id);
static std::pair<Value, Value> emitEdgeRange(IRRewriter &rewriter,
                                             Location loc, WorldAccess &world,
                                             const WorldRelation &relation,
                                             bool in, Value key);
static std::optional<SmallVector<Value>>
emitReadFields(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
               WorldAccess &world, Value id, FlatSymbolRefAttr component,
               ArrayRef<StringAttr> fields, bool trusted);
static std::optional<Value>
emitReadStamp(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
              WorldAccess &world, Value id, const Stamp &stamp,
              bool trusted);
namespace {
/// Bounds that ids are checked against, loaded ahead by a caller that
/// knows they cannot change (see emitLocate).
struct LocateBounds {
  /// Entity counts by archetype index (Rows ids).
  SmallVector<Value> counts;
  /// Slots in use, as an index (slot ids).
  Value slotsInUse;
};
} // namespace
static SmallVector<Value>
emitLocate(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
           WorldAccess &world, Value id,
           function_ref<bool(const WorldArchetype &)> candidate,
           FlatSymbolRefAttr presenceOf, TypeRange results,
           function_ref<SmallVector<Value>(const WorldArchetype &archetype,
                                           Value row, Value present)>
               found,
           function_ref<SmallVector<Value>()> missing,
           const LocateBounds &bounds = {}, bool trusted = false,
           Value location = Value());
namespace {
/// The visited entity's parent in a tree, where the caller has it at hand:
/// its id (in its stored form), its row in an archetype, or its packed
/// location.
struct KnownParent {
  const WorldRelation *relation = nullptr;
  Value id;
  const WorldArchetype *archetype = nullptr;
  Value row;
  Value location;
};
/// Emit, at the insertion point, what `at` emits for the ancestor's
/// archetype and row, and return the values it gives (of `results`): for
/// its one home directly, else in a branch per home.
static SmallVector<Value>
emitAtHome(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
           const Ancestor &ancestor, TypeRange results,
           function_ref<SmallVector<Value>(const WorldArchetype &)> at) {
  ArrayRef<const WorldArchetype *> homes = ancestor.homes;
  if (homes.size() == 1)
    return at(*homes.front());
  OpBuilder::InsertionGuard guard(rewriter);
  scf::IfOp top;
  for (const WorldArchetype *home : homes.drop_back()) {
    Value here = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq, ancestor.where,
        arith::ConstantIntOp::create(rewriter, loc, home->index,
                                     layout.entities.locationBits));
    auto branch = scf::IfOp::create(rewriter, loc, results, here,
                                    /*withElseRegion=*/true);
    if (top && !results.empty())
      scf::YieldOp::create(rewriter, loc, branch.getResults());
    if (!top)
      top = branch;
    rewriter.setInsertionPointToStart(branch.thenBlock());
    SmallVector<Value> values = at(*home);
    if (!results.empty())
      scf::YieldOp::create(rewriter, loc, values);
    rewriter.setInsertionPointToStart(branch.elseBlock());
  }
  SmallVector<Value> values = at(*homes.back());
  if (!results.empty())
    scf::YieldOp::create(rewriter, loc, values);
  return SmallVector<Value>(top.getResults());
}
} // namespace
/// Combine `value` (in its stored form) into a field of the entity at
/// `row` of `target` and record the change for reactive queries.
static void combineAtRow(IRRewriter &rewriter, Location loc,
                         const WorldLayout &layout, WorldAccess &world,
                         const WorldArchetype &target, Value row,
                         StringAttr component, StringAttr field,
                         StringRef rule, Value value, Value id, Value tick);

/// Emit the body of `query` for `entity` of `archetype` at the insertion
/// point, with the query's parameters and outer values mapped by
/// `mapping`. If the query binds components that are optional in the
/// archetype, the body applies only where all of them are present: either
/// it runs for every entity and its stores are masked (branch-free, which
/// keeps the loop vectorisable), or, if the body cannot safely run for
/// absent entities, it is guarded by an `scf.if`.
static void emitQueryBody(IRRewriter &rewriter, QueryOp query,
                          IRMapping mapping, const WorldArchetype &archetype,
                          WorldAccess &world, const WorldLayout &layout,
                          Value entity, Value rows, Value tick, Value seen,
                          bool parallel, bool directApplies = false,
                          KnownParent parent = {},
                          bool marksPending = false) {
  Location loc = query.getLoc();
  ArchetypeOp archetypeOp = archetype.op;
  Value mask;
  // A reactive query applies only to the entities with an event since it
  // last started (`seen`): a stamp newer than that, for any trigger. (A
  // trigger on an ancestor's event joins below, once the ancestor is
  // found.)
  Value fired;
  auto fires = [&](Value newer) {
    fired = fired ? arith::OrIOp::create(rewriter, loc, fired, newer)
                        .getResult()
                  : newer;
  };
  if (seen) {
    for (const Trigger &trigger : getTriggers(query)) {
      // Connected to another parent, perhaps: the relation has the tick.
      if (trigger.kind == Trigger::Connected) {
        // (A sorted archetype has a copy per row, next to what else the
        // query reads of the row.)
        const WorldRelation &tree =
            layout.getRelation(trigger.component.getAttr());
        // Where nothing has been connected since the query last ran,
        // which the relation knows, no entity's tick is looked at.
        Value any = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::sgt,
            memref::LoadOp::create(rewriter, loc, world.connectedTicks(tree),
                                   ValueRange{world.latestConnect(loc)}),
            seen);
        auto ifAny = scf::IfOp::create(rewriter, loc,
                                       TypeRange{rewriter.getI1Type()}, any,
                                       /*withElseRegion=*/true);
        {
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(ifAny.thenBlock());
          Value connected =
              archetype.connectedRowOffset &&
                      archetype.sortedBy == trigger.component.getAttr()
                  ? memref::LoadOp::create(rewriter, loc,
                                           world.connectedRows(archetype),
                                           ValueRange{entity})
                  : memref::LoadOp::create(
                        rewriter, loc, world.connectedTicks(tree),
                        ValueRange{world.entityKey(
                            loc, world.entityId(loc, archetype, entity))});
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::sgt, connected, seen)});
          rewriter.setInsertionPointToStart(ifAny.elseBlock());
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 1)});
        }
        fires(ifAny.getResult(0));
        continue;
      }
      // Another sibling before it than it had, for a trigger before a
      // tree (that sibling's own events join below, with the ancestors').
      if (trigger.where == Trigger::Before ||
          trigger.where == Trigger::After) {
        const WorldRelation &tree =
            layout.getRelation(trigger.via.getAttr());
        if (tree.siblingTicksOffset)
          fires(arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::sgt,
              memref::LoadOp::create(
                  rewriter, loc, world.siblingTicks(tree),
                  ValueRange{world.entityKey(
                      loc, world.entityId(loc, archetype, entity))}),
              seen));
      }
      const WorldColumn *column = archetype.findStamp(getStamp(trigger));
      // (Another entity's, or one that is asked on the way up.)
      if (!column || trigger.via || trigger.onTheWay)
        continue;
      Value stamped = memref::LoadOp::create(
          rewriter, loc, world.stamps(archetype, *column), ValueRange{entity});
      fires(arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::sgt,
                                  stamped, seen));
    }
  }
  // Whether the entity has a component it holds optionally, read once.
  llvm::DenseMap<Attribute, Value> presence;
  auto isPresent = [&](FlatSymbolRefAttr component) {
    Value &present = presence[component];
    if (!present) {
      Value column = world.column(archetype, component.getAttr(),
                                  rewriter.getStringAttr(""));
      Value byte =
          memref::LoadOp::create(rewriter, loc, column, ValueRange{entity});
      Value zero = arith::ConstantIntOp::create(rewriter, loc, 0, 8);
      present = arith::CmpIOp::create(rewriter, loc,
                                      arith::CmpIPredicate::ne, byte, zero);
    }
    return present;
  };
  auto require = [&](Value term) {
    mask = mask ? arith::AndIOp::create(rewriter, loc, mask, term).getResult()
                : term;
  };
  PresenceTest test = getPresenceTest(query, archetypeOp);
  for (FlatSymbolRefAttr component : test.present)
    require(isPresent(component));
  for (FlatSymbolRefAttr component : test.absent)
    require(arith::XOrIOp::create(
        rewriter, loc, isPresent(component),
        arith::ConstantIntOp::create(rewriter, loc, 1, 1)));
  for (const auto &group : test.anyPresent) {
    Value any;
    for (FlatSymbolRefAttr component : group)
      any = any ? arith::OrIOp::create(rewriter, loc, any,
                                       isPresent(component))
                      .getResult()
                : isPresent(component);
    require(any);
  }
  // `ent.has` answers as of the query's start: read before the body runs.
  query.getBody().walk([&](HasOp has) {
    if (archetypeOp.isOptional(has.getComponentAttr()))
      isPresent(has.getComponentAttr());
  });
  // So does an optional ref to a component of the entity's own.
  for (Type type : query.getBody().getArgumentTypes())
    if (auto refType = cast<RefType>(type);
        !refType.isUp() && refType.getIsOptional() &&
        archetypeOp.isOptional(refType.getComponent()))
      isPresent(refType.getComponent());
  // A ref up a relation: find the ancestor it leads to. The body applies
  // only to the entities that have one.
  SmallVector<std::pair<BlockArgument, Ancestor>> ancestors;
  for (BlockArgument arg : query.getBody().getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    if (!refType.isUp())
      continue;
    const WorldRelation &relation =
        layout.getRelation(refType.getVia().getAttr());
    KnownParent known = parent.relation == &relation ? parent : KnownParent();
    Ancestor ancestor;
    // For a trigger on what several steps lead to: whether an entity on
    // the way has been connected to another parent since the query last
    // ran, which makes what the steps lead to another too. (`moved`, of
    // the edges gone along: `walked` is told each one's source.)
    bool tracks = seen && llvm::any_of(getTriggers(query),
                                       [&](const Trigger &trigger) {
                                         return trigger.means(refType);
                                       });
    Value moved;
    auto newlyConnected = [&](const WorldRelation &tree, Value id) -> Value {
      return arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::sgt,
          memref::LoadOp::create(rewriter, loc, world.connectedTicks(tree),
                                 ValueRange{world.entityKey(loc, id)}),
          seen);
    };
    auto walked = [&](const WorldRelation &tree, Value id, Value valid) {
      if (!tracks || !tree.connectedOffset)
        return;
      Value newer =
          arith::AndIOp::create(rewriter, loc, valid, newlyConnected(tree, id));
      moved = moved ? arith::OrIOp::create(rewriter, loc, moved, newer)
                          .getResult()
                    : newer;
    };
    // (The nearest ancestor that has the component is found step by step
    // too where a trigger means it and not every parent has it: on the
    // way it shows whether one has lost or got it.)
    ArrayAttr path = refType.getPath();
    if (!path && tracks && !refType.getIsDirect() && refType.getHops() == 1 &&
        !refType.getIsBefore() && !refType.getIsAfter() &&
        relation.getTrusted(/*target=*/true) != refType.getComponent()) {
      Builder builder(rewriter.getContext());
      path = builder.getArrayAttr({builder.getArrayAttr(
          {builder.getStringAttr("up"), refType.getVia(),
           refType.getComponent()})});
    }
    // Whether the entity `id` has had `component` taken from it, or given
    // to it, since the query last ran.
    auto changedHaving = [&](Value id, FlatSymbolRefAttr component,
                             Trigger::Kind kind) -> Value {
      Stamp stamp{kind, component.getAttr(), rewriter.getStringAttr("")};
      auto stores = [&](const WorldArchetype &home) {
        return home.findStamp(stamp) != nullptr;
      };
      Type i64 = rewriter.getI64Type();
      Value zero = arith::ConstantIntOp::create(rewriter, loc, 0, 64);
      if (llvm::none_of(layout.archetypes, stores))
        return arith::ConstantIntOp::create(rewriter, loc, 0, 1);
      std::optional<Value> byTable = emitReadStamp(
          rewriter, loc, layout, world, id, stamp, /*trusted=*/false);
      Value stamped = byTable ? *byTable : emitLocate(
          rewriter, loc, layout, world, id, stores, FlatSymbolRefAttr(),
          TypeRange{i64},
          [&](const WorldArchetype &home, Value row,
              Value) -> SmallVector<Value> {
            return {memref::LoadOp::create(
                rewriter, loc, world.stamps(home, *home.findStamp(stamp)),
                ValueRange{row})};
          },
          [&]() -> SmallVector<Value> { return {zero}; })[0];
      return arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::sgt,
                                   stamped, seen);
    };
    if (path) {
      // Step by step from the entity: to a parent, or to the nearest
      // ancestor that has what the step names, each along its own tree.
      Type i1 = rewriter.getI1Type();
      Value yes = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
      Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
      Value own = world.entityId(loc, archetype, entity);
      Value at = own, has = yes;
      // Whether the entity `id` is alive and has `component`.
      auto holds = [&](Value id, FlatSymbolRefAttr component) -> Value {
        if (auto byTable = emitReadFields(rewriter, loc, layout, world, id,
                                          component, {}, /*trusted=*/false))
          return byTable->back();
        return emitLocate(
            rewriter, loc, layout, world, id,
            [](const WorldArchetype &) { return true; }, component,
            TypeRange{i1},
            [&](const WorldArchetype &home, Value,
                Value present) -> SmallVector<Value> {
              if (!ArchetypeOp(home.op).contains(component))
                return {no};
              return {present ? present : yes};
            },
            [&]() -> SmallVector<Value> { return {no}; })[0];
      };
      for (Attribute attr : path) {
        auto step = cast<ArrayAttr>(attr);
        const WorldRelation &tree =
            layout.getRelation(cast<FlatSymbolRefAttr>(step[1]).getAttr());
        // (Where a step before led nowhere, the entity is asked in its
        // place, to no effect.)
        Value from = arith::SelectOp::create(rewriter, loc, has, at, own);
        walked(tree, from, has);
        bool climbs = tracks && tree.connectedOffset;
        StringRef kind = cast<StringAttr>(step[0]).getValue();
        if (kind == "before" || kind == "after") {
          // To the sibling before or after: by the tree's links, or what
          // the sort noted for the entity's key.
          bool after = kind == "after";
          Value key = world.entityKey(loc, from);
          Value more, next;
          if (tree.linked) {
            Value mine = arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::eq,
                memref::LoadOp::create(rewriter, loc,
                                       world.edgeIds(tree, /*source=*/true),
                                       ValueRange{key}),
                world.slotOwner(loc, from));
            Value link = memref::LoadOp::create(
                rewriter, loc,
                world.treeLinks(tree, after ? tree.nextSiblingOffset
                                            : tree.previousSiblingOffset),
                ValueRange{key});
            more = arith::AndIOp::create(
                rewriter, loc, mine,
                arith::CmpIOp::create(
                    rewriter, loc, arith::CmpIPredicate::ne, link,
                    arith::ConstantIntOp::create(rewriter, loc, 0,
                                                 tree.offsetBits)));
            Value slot = arith::SelectOp::create(
                rewriter, loc, more,
                arith::SubIOp::create(
                    rewriter, loc, world.toIndex(loc, link),
                    arith::ConstantIndexOp::create(rewriter, loc, 1)),
                arith::ConstantIndexOp::create(rewriter, loc, 0));
            next = world.ownerId(
                loc, memref::LoadOp::create(
                         rewriter, loc, world.edgeIds(tree, /*source=*/true),
                         ValueRange{slot}));
          } else {
            next = memref::LoadOp::create(rewriter, loc,
                                          world.siblingIds(tree, after),
                                          ValueRange{key});
            more = arith::CmpIOp::create(rewriter, loc,
                                         arith::CmpIPredicate::ne, next,
                                         world.noEntity(loc));
          }
          // (Another sibling there than when the query last ran.)
          if (tracks && tree.siblingTicksOffset) {
            Value newer = arith::AndIOp::create(
                rewriter, loc, has,
                arith::CmpIOp::create(
                    rewriter, loc, arith::CmpIPredicate::sgt,
                    memref::LoadOp::create(rewriter, loc,
                                           world.siblingTicks(tree),
                                           ValueRange{key}),
                    seen));
            moved = moved ? arith::OrIOp::create(rewriter, loc, moved, newer)
                                .getResult()
                          : newer;
          }
          has = arith::AndIOp::create(rewriter, loc, has, more);
          at = next;
          continue;
        }
        if (kind == "parent") {
          auto [more, next] =
              emitParent(rewriter, loc, layout, world, tree, from);
          has = arith::AndIOp::create(rewriter, loc, has, more);
          at = next;
          continue;
        }
        // Up: parent by parent, until one has all the step names.
        auto [first, parentOf] =
            emitParent(rewriter, loc, layout, world, tree, from);
        auto climb = scf::WhileOp::create(
            rewriter, loc, TypeRange{world.idType(), i1, i1, i1},
            ValueRange{parentOf, no,
                       arith::AndIOp::create(rewriter, loc, has, first), no},
            [&](OpBuilder &builder, Location, ValueRange state) {
              scf::ConditionOp::create(builder, loc, state[2], state);
            },
            [&](OpBuilder &, Location, ValueRange state) {
              Value all = yes;
              for (Attribute part : step.getValue().drop_front(2))
                all = arith::AndIOp::create(
                    rewriter, loc, all,
                    holds(state[0], cast<FlatSymbolRefAttr>(part)));
              auto further = scf::IfOp::create(
                  rewriter, loc, TypeRange{world.idType(), i1, i1}, all,
                  /*withElseRegion=*/true);
              {
                OpBuilder::InsertionGuard guard(rewriter);
                rewriter.setInsertionPointToStart(further.thenBlock());
                scf::YieldOp::create(rewriter, loc,
                                     ValueRange{state[0], no, state[3]});
                rewriter.setInsertionPointToStart(further.elseBlock());
                auto [more, next] =
                    emitParent(rewriter, loc, layout, world, tree, state[0]);
                // (On past this one: along its edge. And if it has lost
                // what the step asks for, it was where the step ended.)
                Value newer = state[3];
                if (climbs)
                  newer = arith::OrIOp::create(
                      rewriter, loc, newer,
                      arith::AndIOp::create(rewriter, loc, more,
                                            newlyConnected(tree, state[0])));
                if (tracks)
                  for (Attribute part : step.getValue().drop_front(2))
                    newer = arith::OrIOp::create(
                        rewriter, loc, newer,
                        changedHaving(state[0],
                                      cast<FlatSymbolRefAttr>(part),
                                      Trigger::Removed));
                scf::YieldOp::create(rewriter, loc,
                                     ValueRange{next, more, newer});
              }
              scf::YieldOp::create(
                  rewriter, loc,
                  ValueRange{further.getResult(0), all, further.getResult(1),
                             further.getResult(2)});
            });
        if (tracks) {
          // (And where it ends now: one that has just got what it asks
          // for was passed before.)
          Value other = climb.getResult(3);
          Value found =
              arith::AndIOp::create(rewriter, loc, has, climb.getResult(1));
          Value end = arith::SelectOp::create(rewriter, loc, found,
                                              climb.getResult(0), own);
          for (Attribute part : step.getValue().drop_front(2))
            other = arith::OrIOp::create(
                rewriter, loc, other,
                arith::AndIOp::create(
                    rewriter, loc, found,
                    changedHaving(end, cast<FlatSymbolRefAttr>(part),
                                  Trigger::Added)));
          moved = moved ? arith::OrIOp::create(rewriter, loc, moved, other)
                              .getResult()
                        : other;
        }
        has = arith::AndIOp::create(rewriter, loc, has, climb.getResult(1));
        at = climb.getResult(0);
      }
      at = arith::SelectOp::create(rewriter, loc, has, at,
                                   world.noEntity(loc));
      ancestor = {arith::AndIOp::create(rewriter, loc, has,
                                        holds(at, refType.getComponent())),
                  at, /*trusted=*/false, {}, Value(), Value()};
    } else if (refType.getIsBefore() || refType.getIsAfter()) {
      // The sibling before or after, by the tree's links.
      ancestor = emitSibling(rewriter, loc, layout, world, relation,
                             refType.getComponent(), archetype, entity,
                             refType.getIsAfter());
    } else if (refType.getHops() != 1) {
      // More than one step up: from the entity, parent by parent.
      ancestor = emitAncestor(rewriter, loc, layout, world, relation,
                              refType.getComponent(),
                              world.entityId(loc, archetype, entity), Value(),
                              /*direct=*/true, refType.getHops());
      // (The parents on the way, each along its own edge.)
      if (tracks && relation.connectedOffset) {
        Value own = world.entityId(loc, archetype, entity);
        Value at = own;
        Value valid = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
        for (unsigned step = 1; step < refType.getHops(); ++step) {
          auto [more, next] =
              emitParent(rewriter, loc, layout, world, relation, at);
          valid = arith::AndIOp::create(rewriter, loc, valid, more);
          at = arith::SelectOp::create(rewriter, loc, valid, next, own);
          walked(relation, at, valid);
        }
      }
    } else if (known.row &&
        ArchetypeOp(known.archetype->op).contains(refType.getComponent()) &&
        !ArchetypeOp(known.archetype->op)
             .isOptional(refType.getComponent())) {
      // The parent's row, in an archetype that always has the component.
      ancestor = {Value(), Value(), /*trusted=*/true, {known.archetype},
                  Value(), known.row};
    } else if (known.location) {
      // The parent is a target of the tree's edges: where every archetype
      // that can hold one always has the component, it is the ancestor,
      // at the row its location names. Otherwise the ancestor is searched
      // for.
      RelationOp relationOp = relation.op;
      SmallVector<const WorldArchetype *, 2> homes;
      bool always = true;
      for (const WorldArchetype &home : layout.archetypes) {
        ArchetypeOp homeOp = home.op;
        if (!homeOp.contains(relationOp.getToAttr()))
          continue;
        homes.push_back(&home);
        always &= homeOp.contains(refType.getComponent()) &&
                  !homeOp.isOptional(refType.getComponent());
      }
      if (always && !homes.empty()) {
        auto [where, row] = world.unpackLocation(loc, known.location);
        ancestor = {Value(), Value(), /*trusted=*/true, homes, where, row};
      } else {
        ancestor = emitAncestor(rewriter, loc, layout, world, relation,
                                refType.getComponent(),
                                world.entityId(loc, archetype, entity),
                                known.id, refType.getIsDirect());
      }
    } else {
      if (known.row)
        known.id = memref::LoadOp::create(rewriter, loc,
                                          world.ids(*known.archetype),
                                          ValueRange{known.row});
      ancestor = emitAncestor(rewriter, loc, layout, world, relation,
                              refType.getComponent(),
                              world.entityId(loc, archetype, entity),
                              known.id, refType.getIsDirect());
    }
    // Null: every entity the caller visits has one. (Through an optional
    // ref the body sees whether there is one, and runs either way.)
    if (ancestor.found && !refType.getIsOptional())
      require(ancestor.found);
    ancestors.push_back({arg, ancestor});
    // The ancestor's events, for a trigger `up` this tree: its stamp, read
    // where it is (0 where the entity has no such ancestor). An ancestor
    // this query has already visited has what it did there in its stamp.
    if (seen)
      for (const Trigger &trigger : getTriggers(query)) {
        if (!trigger.means(refType))
          continue;
        if (moved) {
          fires(moved);
          moved = Value();
        }
        Stamp stamp = getStamp(trigger);
        Type i64 = rewriter.getI64Type();
        auto stampAt = [&](const WorldArchetype &home, Value row) -> Value {
          const WorldColumn *column = home.findStamp(stamp);
          assert(column && "an archetype holding the component of a trigger "
                           "up a tree stores its stamp");
          return memref::LoadOp::create(rewriter, loc,
                                        world.stamps(home, *column),
                                        ValueRange{row});
        };
        auto read = [&]() -> Value {
          if (ancestor.row)
            return emitAtHome(rewriter, loc, layout, ancestor, TypeRange{i64},
                              [&](const WorldArchetype &home)
                                  -> SmallVector<Value> {
                                return {stampAt(home, ancestor.row)};
                              })[0];
          FlatSymbolRefAttr component = refType.getComponent();
          // (No entity has the component: none has had an event of it.)
          if (llvm::none_of(layout.archetypes,
                            [&](const WorldArchetype &home) {
                              return ArchetypeOp(home.op).contains(component);
                            }))
            return arith::ConstantIntOp::create(rewriter, loc, 0, 64);
          if (auto byTable =
                  emitReadStamp(rewriter, loc, layout, world, ancestor.id,
                                stamp, ancestor.trusted))
            return *byTable;
          return emitLocate(
              rewriter, loc, layout, world, ancestor.id,
              [&](const WorldArchetype &home) {
                return ArchetypeOp(home.op).contains(component);
              },
              component, TypeRange{i64},
              [&](const WorldArchetype &home, Value row,
                  Value) -> SmallVector<Value> {
                return {stampAt(home, row)};
              },
              [&]() -> SmallVector<Value> {
                return {arith::ConstantIntOp::create(rewriter, loc, 0, 64)};
              },
              LocateBounds(), ancestor.trusted)[0];
        };
        Value stamped;
        if (ancestor.found) {
          auto ifFound = scf::IfOp::create(rewriter, loc, TypeRange{i64},
                                           ancestor.found,
                                           /*withElseRegion=*/true);
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(ifFound.thenBlock());
          scf::YieldOp::create(rewriter, loc, ValueRange{read()});
          rewriter.setInsertionPointToStart(ifFound.elseBlock());
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 64)});
          stamped = ifFound.getResult(0);
        } else {
          stamped = read();
        }
        // What this query did to an ancestor the last time it ran, it
        // has passed down then: those events carry the tick after the one
        // it started at and no others do (see lowerCascade), so an
        // ancestor's event counts from the tick after that. (A query that
        // has not run has passed nothing down.)
        Value zero = arith::ConstantIntOp::create(rewriter, loc, 0, 64);
        Value one = arith::ConstantIntOp::create(rewriter, loc, 1, 64);
        Value passed = arith::SelectOp::create(
            rewriter, loc,
            arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                  seen, zero),
            zero, arith::AddIOp::create(rewriter, loc, seen, one));
        // (The sibling after is visited after the entity: what this query
        // did to it the last time has not been taken in, and counts.)
        fires(arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::sgt, stamped,
            trigger.where == Trigger::After ? seen : passed));
      }
  }
  // A trigger down a tree: an event of one of the entity's children,
  // each asked in turn until one has had one, or a child gained or lost.
  if (seen)
    for (const Trigger &trigger : getTriggers(query)) {
      if (trigger.where != Trigger::Down)
        continue;
      const WorldRelation &relation =
          layout.getRelation(trigger.via.getAttr());
      Type i64 = rewriter.getI64Type();
      Type i1 = rewriter.getI1Type();
      Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
      auto load = [&](Value column, Value at) -> Value {
        return memref::LoadOp::create(rewriter, loc, column, ValueRange{at});
      };
      auto constant = [&](int64_t value, unsigned bits) -> Value {
        return arith::ConstantIntOp::create(rewriter, loc, value, bits);
      };
      Value key = world.entityKey(loc, world.entityId(loc, archetype, entity));
      if (relation.childTicksOffset)
        fires(arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::sgt,
                                    load(world.childTicks(relation), key),
                                    seen));
      // (What this query did to a child the last time, it has taken in
      // then: see the trigger up a tree above. Parents first it has not,
      // the child coming after the entity: that counts.)
      Value passed = arith::SelectOp::create(
          rewriter, loc,
          arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq, seen,
                                constant(0, 64)),
          constant(0, 64),
          arith::AddIOp::create(rewriter, loc, seen, constant(1, 64)));
      if (!query.isLeavesFirst())
        passed = seen;
      Stamp stamp = getStamp(trigger);
      FlatSymbolRefAttr component = trigger.component;
      // The stamp of the child `childId`, 0 if it has none.
      auto stampOf = [&](Value childId) -> Value {
        if (llvm::none_of(layout.archetypes, [&](const WorldArchetype &home) {
              return home.findStamp(stamp) != nullptr;
            }))
          return constant(0, 64);
        if (auto byTable = emitReadStamp(rewriter, loc, layout, world,
                                         childId, stamp, /*trusted=*/false))
          return *byTable;
        return emitLocate(
            rewriter, loc, layout, world, childId,
            [&](const WorldArchetype &home) {
              return home.findStamp(stamp) != nullptr;
            },
            component, TypeRange{i64},
            [&](const WorldArchetype &home, Value row,
                Value) -> SmallVector<Value> {
              return {load(world.stamps(home, *home.findStamp(stamp)), row)};
            },
            [&]() -> SmallVector<Value> { return {constant(0, 64)}; })[0];
      };
      if (!relation.linked) {
        // A tree without links: the children are the sources of the edges
        // to the entity, which the relation has together.
        Value id = world.entityId(loc, archetype, entity);
        auto [begin, end] =
            emitEdgeRange(rewriter, loc, world, relation, /*in=*/true, key);
        auto edges = scf::ForOp::create(rewriter, loc, begin, end, one,
                                        ValueRange{constant(0, 1)});
        {
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(edges.getBody());
          Value position = edges.getInductionVar();
          Value edge = relation.isSorted(/*in=*/true)
                           ? position
                           : world.toIndex(
                                 loc, load(world.indexEdges(relation),
                                           position));
          // (A slot may have been reused since the edges were sorted.)
          Value mine = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::eq,
              load(world.edgeIds(relation, /*source=*/false), edge), id);
          Value newer = arith::AndIOp::create(
              rewriter, loc, mine,
              arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::sgt,
                  stampOf(load(world.edgeIds(relation, /*source=*/true),
                               edge)),
                  passed));
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::OrIOp::create(rewriter, loc,
                                              edges.getRegionIterArg(0),
                                              newer)});
        }
        fires(edges.getResult(0));
        continue;
      }
      Value nextSibling =
          world.treeLinks(relation, relation.nextSiblingOffset);
      auto children = scf::WhileOp::create(
          rewriter, loc, TypeRange{world.offsetType(relation), i1},
          ValueRange{load(world.treeLinks(relation, relation.firstChildOffset),
                          key),
                     constant(0, 1)},
          [&](OpBuilder &, Location, ValueRange state) {
            Value more = arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::ne, state[0],
                constant(0, relation.offsetBits));
            scf::ConditionOp::create(
                rewriter, loc,
                arith::AndIOp::create(
                    rewriter, loc, more,
                    arith::XOrIOp::create(rewriter, loc, state[1],
                                          constant(1, 1))),
                state);
          },
          [&](OpBuilder &, Location, ValueRange state) {
            Value child = arith::SubIOp::create(
                rewriter, loc, world.toIndex(loc, state[0]), one);
            Value childId = world.ownerId(
                loc, load(world.edgeIds(relation, /*source=*/true), child));
            Value stamped = llvm::none_of(
                                layout.archetypes,
                                [&](const WorldArchetype &home) {
                                  return home.findStamp(stamp) != nullptr;
                                })
                                ? arith::ConstantIntOp::create(rewriter, loc,
                                                               0, 64)
                                      .getResult()
                                : emitLocate(
                rewriter, loc, layout, world, childId,
                [&](const WorldArchetype &home) {
                  return home.findStamp(stamp) != nullptr;
                },
                component, TypeRange{i64},
                [&](const WorldArchetype &home, Value row,
                    Value) -> SmallVector<Value> {
                  return {load(world.stamps(home, *home.findStamp(stamp)),
                               row)};
                },
                [&]() -> SmallVector<Value> { return {constant(0, 64)}; })[0];
            scf::YieldOp::create(
                rewriter, loc,
                ValueRange{load(nextSibling, child),
                           arith::CmpIOp::create(rewriter, loc,
                                                 arith::CmpIPredicate::sgt,
                                                 stamped, passed)});
          });
      fires(children.getResult(1));
    }
  // Reactive: only where a trigger fired (nowhere, if none can here).
  if (seen)
    require(fired ? fired
                  : arith::ConstantIntOp::create(rewriter, loc, 0, 1)
                        .getResult());

  OpBuilder::InsertionGuard guard(rewriter);
  // Without an ancestor there is nothing to read: such a body never runs
  // masked.
  bool guarded = mask && (!canRunForAbsentEntities(query) ||
                          isStructuralFor(query, archetypeOp) ||
                          !ancestors.empty());
  assert((mask || ancestors.empty() ||
          llvm::all_of(ancestors, [](auto &entry) {
            return !entry.second.found ||
                   cast<RefType>(entry.first.getType()).getIsOptional();
          })) && "a body reading an ancestor runs unguarded");
  // An apply or connect that may not run for this entity (it is under an
  // `if`, or the body is guarded) must still leave its slot saying "no
  // target". So must an apply in an edge loop, whose row slot says whether
  // the loop ran.
  query.getBody().walk([&](Operation *apply) {
    if (!isa<ApplyOp, AccumulateOp, ConnectOp>(apply) ||
        appliesDirectly(apply, directApplies))
      return;
    bool inEdges = apply->getParentOfType<EdgesOp>() != nullptr;
    if (!inEdges && !guarded &&
        apply->getBlock() == &query.getBody().front())
      return;
    Value ids = isa<ConnectOp>(apply)
                    ? world.connectBuffer(cast<ConnectOp>(apply), archetype)
                          .sources
                    : world.applyBuffer(apply, archetype).first;
    memref::StoreOp::create(rewriter, loc, world.noEntity(loc), ids,
                            ValueRange{entity});
  });
  if (guarded) {
    auto branch = scf::IfOp::create(rewriter, loc, mask);
    rewriter.setInsertionPointToStart(branch.thenBlock());
    mask = Value();
  }
  SmallVector<Operation *> roots;
  for (Operation &op : query.getBody().front().without_terminator())
    roots.push_back(rewriter.clone(op, mapping));
  // An optional ref to a component of the entity's own leads somewhere
  // where the entity has the component: what its archetype says, or for
  // a component held optionally the entity itself. Where the archetype
  // has no such column, what is read is nothing.
  for (BlockArgument arg : query.getBody().getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    if (refType.isUp() || !refType.getIsOptional())
      continue;
    FlatSymbolRefAttr component = refType.getComponent();
    bool held = archetypeOp.contains(component);
    SmallVector<Operation *> uses;
    for (Operation *root : roots)
      root->walk([&](Operation *op) {
        if (auto bound = dyn_cast<BoundOp>(op);
            bound && bound.getRef() == arg)
          uses.push_back(op);
        else if (auto get = dyn_cast<GetOp>(op);
                 get && get.getRef() == arg && !held)
          uses.push_back(op);
      });
    for (Operation *op : uses) {
      rewriter.setInsertionPoint(op);
      Value answer;
      if (isa<BoundOp>(op)) {
        answer = presence.lookup(component);
        if (!answer)
          answer = arith::ConstantIntOp::create(rewriter, op->getLoc(), held, 1);
      } else {
        Type stored = world.storageType(op->getResult(0).getType());
        answer = world.fromStorage(
            op->getLoc(),
            arith::ConstantOp::create(rewriter, op->getLoc(),
                                      cast<TypedAttr>(rewriter.getZeroAttr(stored))),
            op->getResult(0).getType());
      }
      llvm::erase(roots, op);
      rewriter.replaceOp(op, answer);
    }
  }
  // Reading through a ref to an ancestor is a lookup of the ancestor,
  // which is known to be found.
  for (auto &[arg, ancestor] : ancestors) {
    // Whether an optional ref leads anywhere.
    bool mayBeNone = cast<RefType>(arg.getType()).getIsOptional() &&
                     static_cast<bool>(ancestor.found);
    SmallVector<BoundOp> asked;
    for (Operation *root : roots)
      root->walk([&, arg = arg](BoundOp bound) {
        if (bound.getRef() == arg)
          asked.push_back(bound);
      });
    for (BoundOp bound : asked) {
      Value answer = ancestor.found;
      if (!answer) {
        rewriter.setInsertionPoint(bound);
        answer = arith::ConstantIntOp::create(rewriter, bound.getLoc(), 1, 1);
      }
      Operation *op = bound;
      llvm::erase(roots, op);
      rewriter.replaceOp(bound, answer);
    }
    // The entity the ref leads to: its id, or the one at its row.
    SmallVector<OtherOp> others;
    for (Operation *root : roots)
      root->walk([&, arg = arg](OtherOp other) {
        if (other.getRef() == arg)
          others.push_back(other);
      });
    for (OtherOp other : others) {
      rewriter.setInsertionPoint(other);
      Value id = ancestor.id;
      if (!id)
        id = emitAtHome(rewriter, other.getLoc(), layout, ancestor,
                        TypeRange{world.idType()},
                        [&](const WorldArchetype &home) -> SmallVector<Value> {
                          return {world.entityId(other.getLoc(), home,
                                                 ancestor.row)};
                        })[0];
      if (ancestor.found)
        id = arith::SelectOp::create(rewriter, other.getLoc(), ancestor.found,
                                     id, world.noEntity(other.getLoc()));
      Operation *op = other;
      llvm::erase(roots, op);
      rewriter.replaceOp(other,
                         world.fromStorage(other.getLoc(), id,
                                           EntityType::get(
                                               rewriter.getContext())));
    }
    SmallVector<GetOp> gets;
    for (Operation *root : roots)
      root->walk([&, arg = arg](GetOp get) {
        if (get.getRef() == arg)
          gets.push_back(get);
      });
    // Where the ancestor is known by its id and several of its fields are
    // read, it is found once, before the body, and all are read there:
    // finding it is most of what a read costs, in time and in code.
    if (!ancestor.row && gets.size() > 1 && !roots.empty()) {
      FlatSymbolRefAttr component =
          cast<RefType>(arg.getType()).getComponent();
      SmallVector<StringAttr> fields;
      SmallVector<Type> types;
      for (GetOp get : gets)
        if (!llvm::is_contained(fields, get.getFieldAttr())) {
          fields.push_back(get.getFieldAttr());
          types.push_back(world.storageType(get.getType()));
        }
      Location at = gets.front().getLoc();
      rewriter.setInsertionPoint(roots.front());
      auto nothing = [&]() -> SmallVector<Value> {
        SmallVector<Value> zeros;
        for (Type type : types)
          zeros.push_back(arith::ConstantOp::create(
              rewriter, at, cast<TypedAttr>(rewriter.getZeroAttr(type))));
        return zeros;
      };
      auto holds = [&](const WorldArchetype &home) {
        return ArchetypeOp(home.op).contains(component);
      };
      std::optional<SmallVector<Value>> byTable =
          emitReadFields(rewriter, at, layout, world, ancestor.id, component,
                         fields, ancestor.trusted && !mayBeNone);
      if (byTable)
        byTable->pop_back();
      SmallVector<Value> values =
          byTable ? *byTable
          : llvm::none_of(layout.archetypes, holds)
              ? nothing()
              : emitLocate(
                    rewriter, at, layout, world, ancestor.id, holds,
                    FlatSymbolRefAttr(), types,
                    [&](const WorldArchetype &home, Value row,
                        Value) -> SmallVector<Value> {
                      SmallVector<Value> read;
                      for (StringAttr field : fields)
                        read.push_back(memref::LoadOp::create(
                            rewriter, at,
                            world.column(home, component.getAttr(), field),
                            ValueRange{row}));
                      return read;
                    },
                    nothing, LocateBounds(), ancestor.trusted && !mayBeNone);
      SmallVector<Value> converted;
      for (GetOp get : gets)
        converted.push_back(world.fromStorage(
            get.getLoc(),
            values[llvm::find(fields, get.getFieldAttr()) - fields.begin()],
            get.getType()));
      for (auto [get, value] : llvm::zip(gets, converted)) {
        Operation *op = get;
        llvm::erase(roots, op);
        rewriter.replaceOp(get, value);
      }
      gets.clear();
    }
    for (GetOp get : gets) {
      rewriter.setInsertionPoint(get);
      Operation *op = get;
      if (ancestor.row) {
        Value value = emitAtHome(
            rewriter, get.getLoc(), layout, ancestor,
            TypeRange{world.storageType(get.getType())},
            [&](const WorldArchetype &home) -> SmallVector<Value> {
              return {memref::LoadOp::create(
                  rewriter, get.getLoc(),
                  world.column(
                      home,
                      cast<RefType>(arg.getType()).getComponent().getAttr(),
                      get.getFieldAttr()),
                  ValueRange{ancestor.row})};
            })[0];
        auto *at = llvm::find(roots, op);
        if (at != roots.end())
          *at = value.getDefiningOp();
        rewriter.replaceOp(
            get, world.fromStorage(get.getLoc(), value, get.getType()));
        continue;
      }
      auto lookup = LookupOp::create(
          rewriter, get.getLoc(), get.getType(), rewriter.getI1Type(),
          world.fromStorage(get.getLoc(), ancestor.id,
                            EntityType::get(rewriter.getContext())),
          cast<RefType>(arg.getType()).getComponent(), get.getFieldAttr());
      // (Where there may be none, the lookup is of no entity, and checks.)
      if (ancestor.trusted && !mayBeNone)
        lookup->setAttr(kTrustedAttr, rewriter.getUnitAttr());
      auto *at = llvm::find(roots, op);
      if (at != roots.end())
        *at = lookup;
      rewriter.replaceOp(get, lookup.getValue());
    }
    // Combining through it is an apply to the ancestor. A cascading query
    // visits the entities in the order its values are combined in, and
    // nothing in it sees the field of another entity, so it combines as it
    // goes.
    SmallVector<CombineOp> combines;
    for (Operation *root : roots)
      root->walk([&, arg = arg](CombineOp combine) {
        if (combine.getRef() == arg)
          combines.push_back(combine);
      });
    for (CombineOp combine : combines) {
      assert(directApplies && "an ancestor is combined into in order");
      rewriter.setInsertionPoint(combine);
      Operation *op = combine;
      if (ancestor.row) {
        Value value = world.toStorage(combine.getLoc(), combine.getValue());
        emitAtHome(rewriter, combine.getLoc(), layout, ancestor, TypeRange{},
                   [&](const WorldArchetype &home) -> SmallVector<Value> {
                     combineAtRow(
                         rewriter, combine.getLoc(), layout, world, home,
                         ancestor.row,
                         cast<RefType>(arg.getType())
                             .getComponent()
                             .getAttr(),
                         combine.getFieldAttr(), combine.getRule(), value,
                         Value(), tick);
                     return {};
                   });
        llvm::erase(roots, op);
        rewriter.eraseOp(combine);
        continue;
      }
      auto apply = ApplyOp::create(
          rewriter, combine.getLoc(),
          world.fromStorage(combine.getLoc(), ancestor.id,
                            EntityType::get(rewriter.getContext())),
          cast<RefType>(arg.getType()).getComponent(), combine.getFieldAttr(),
          combine.getRuleAttr(), combine.getValue());
      apply->setAttr(kUnobservedAttr, rewriter.getUnitAttr());
      if (ancestor.trusted)
        apply->setAttr(kTrustedAttr, rewriter.getUnitAttr());
      auto *at = llvm::find(roots, op);
      if (at != roots.end())
        *at = apply;
      rewriter.eraseOp(combine);
    }
  }
  SmallVector<HasOp> tests;
  for (Operation *root : roots)
    root->walk([&](HasOp has) { tests.push_back(has); });
  for (HasOp has : tests) {
    FlatSymbolRefAttr component = has.getComponentAttr();
    Value answer = presence.lookup(component);
    if (!answer) {
      rewriter.setInsertionPoint(has);
      answer = arith::ConstantIntOp::create(
          rewriter, has.getLoc(), archetypeOp.contains(component), 1);
    }
    rewriter.replaceOp(has, answer);
  }
  llvm::erase_if(roots, [&](Operation *root) {
    return llvm::is_contained(tests, root);
  });
  SmallVector<EdgesOp> edgeLoops;
  for (Operation *root : roots)
    root->walk([&](EdgesOp edges) { edgeLoops.push_back(edges); });
  for (EdgesOp edges : edgeLoops) {
    Operation *op = edges;
    Operation *loop = lowerEdges(rewriter, edges, archetype, world, layout,
                                 entity, tick, directApplies);
    // (A loop that already carries a value, a linked tree's walk from
    // child to child, is left as it is.)
    auto forLoop = cast<scf::ForOp>(loop);
    SmallVector<Operation *> lowered =
        forLoop.getNumRegionIterArgs() == 0
            ? carryOwnFields(rewriter, forLoop)
            : SmallVector<Operation *>{loop};
    auto *at = llvm::find(roots, op);
    if (at != roots.end()) {
      at = roots.erase(at);
      roots.insert(at, lowered.begin(), lowered.end());
    }
  }
  lowerAccesses(rewriter, roots, archetype, world, layout, entity, rows, mask,
                tick, parallel, directApplies, marksPending);
}

/// Append the entity at `row` of `source` to `move.target`, as the move
/// says: shared components are copied, an added component takes the values
/// listed at pending slot `slot`, and optional components the entity did
/// not have in `source` start absent. Updates the entity's location.
static void applyMove(IRRewriter &rewriter, Location loc,
                      const WorldLayout &layout, const WorldArchetype &source,
                      const WorldMove &move, WorldAccess &world, Value row,
                      Value slot, Value id, Value tick) {
  const WorldArchetype *target = nullptr;
  for (const WorldArchetype &entry : layout.archetypes)
    if (entry.op == move.target)
      target = &entry;
  ArchetypeOp sourceOp = source.op, targetOp = target->op;
  Value to = world.count(loc, *target);
  Value capacity =
      arith::ConstantIndexOp::create(rewriter, loc, target->capacity);
  cf::AssertOp::create(
      rewriter, loc,
      arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult, to,
                            capacity),
      rewriter.getStringAttr("moving an entity exceeds the capacity of @" +
                             targetOp.getSymName()));
  for (const WorldColumn &column : target->columns) {
    auto component = FlatSymbolRefAttr::get(column.component);
    Value value;
    if (column.isStamp()) {
      // Stamps move with the entity. Where the source does not store one,
      // the entity gains or loses the component by this move: that is the
      // event (see StampPlan).
      if (const WorldColumn *carried = source.findStamp(*column.stamp)) {
        value = memref::LoadOp::create(
            rewriter, loc, world.stamps(source, *carried), ValueRange{row});
      } else {
        value = tick;
        if (const WorldLog *log = layout.findLog(*column.stamp))
          appendToLog(rewriter, loc, world, *log,
                      segmentOf(rewriter, loc, world, *log, to, Value()), id,
                      tick, Value(), Value(), /*atomic=*/false);
      }
      memref::StoreOp::create(rewriter, loc, value,
                              world.stamps(*target, column), ValueRange{to});
      continue;
    }
    if (column.isPresence()) {
      // Presence in the target: carried over where the source has the
      // component optionally, present where the source always has it or
      // it is being added, absent otherwise.
      if (sourceOp.isOptional(component))
        value = memref::LoadOp::create(
            rewriter, loc, world.column(source, column.component, column.field),
            ValueRange{row});
      else
        value = arith::ConstantIntOp::create(
            rewriter, loc,
            sourceOp.contains(component) ||
                (move.add && column.component == move.component),
            8);
    } else if (sourceOp.contains(component)) {
      value = memref::LoadOp::create(
          rewriter, loc, world.column(source, column.component, column.field),
          ValueRange{row});
    } else if (move.add && column.component == move.component) {
      const WorldColumn *values = nullptr;
      for (const WorldColumn &entry : move.values)
        if (entry.field == column.field)
          values = &entry;
      value = memref::LoadOp::create(
          rewriter, loc, world.moveValues(source, *values), ValueRange{slot});
    } else {
      continue; // an optional component the entity did not have
    }
    memref::StoreOp::create(
        rewriter, loc, value,
        world.column(*target, column.component, column.field), ValueRange{to});
  }
  memref::StoreOp::create(rewriter, loc, id, world.ids(*target),
                          ValueRange{to});
  world.setLocation(loc, world.idSlot(loc, id), *target, to);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  world.setCount(loc, *target, arith::AddIOp::create(rewriter, loc, to, one));
}

/// The name of the function that takes the edges of and to a despawned
/// entity out of `relation`, a linked tree: (the entity's id, the arena).
static std::string dropFunctionName(const WorldRelation &relation) {
  return ("ent_drop_" + RelationOp(relation.op).getSymName()).str();
}

/// Whether entities of `archetype` can be an end of `relation`'s edges:
/// it holds what the relation says its sources or its targets have, or
/// the relation does not say.
static bool canHoldEnd(const WorldArchetype &archetype,
                       const WorldRelation &relation) {
  RelationOp relationOp = relation.op;
  ArchetypeOp archetypeOp = archetype.op;
  for (bool target : {false, true}) {
    FlatSymbolRefAttr component = relationOp.getEndpoint(target);
    if (!component || archetypeOp.contains(component))
      return true;
  }
  return false;
}

/// Apply the rows listed as pending for `archetype`, at the insertion
/// point, last listed first: despawn (free the id) or move the entity to
/// another archetype, then remove the row by moving the archetype's last
/// row into it (swap-remove) and updating that entity's location. The list
/// is in ascending row order, since the query visited rows in order, so no
/// listed row is moved before it is removed; rows appended by moves into
/// this archetype come after all of them.
static void applyPending(IRRewriter &rewriter, Location loc,
                         const WorldLayout &layout,
                         const WorldArchetype &archetype, WorldAccess &world,
                         Value tick) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value counter = world.pendingCount(archetype);
  Value pending = world.toIndex(
      loc, memref::LoadOp::create(rewriter, loc, counter, ValueRange{zero}));
  auto loop = scf::ForOp::create(rewriter, loc, zero, pending, one);
  {
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    Value slot = arith::SubIOp::create(
        rewriter, loc, arith::SubIOp::create(rewriter, loc, pending, one),
        loop.getInductionVar());
    Value row = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, world.pendingList(archetype),
                                    ValueRange{slot}));
    Value id = memref::LoadOp::create(rewriter, loc, world.ids(archetype),
                                      ValueRange{row});
    Value action = archetype.pendingActionOffset
                       ? memref::LoadOp::create(rewriter, loc,
                                                world.pendingActions(archetype),
                                                ValueRange{slot})
                             .getResult()
                       : Value();
    auto is = [&](unsigned code) -> Value {
      if (!action)
        return arith::ConstantIntOp::create(rewriter, loc, code == 0, 1);
      return arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::eq, action,
          arith::ConstantIntOp::create(rewriter, loc, code, 32));
    };
    if (!layout.entities.hasGenerations()) {
      // Nothing is ever despawned (the entity scheme has no generations);
      // every listed row is a move.
    } else {
      // A despawned entity's edges in the trees that keep a slot for it
      // go with it, so that an edge of such a tree never has a dead end.
      auto despawnEntity = [&] {
        for (const WorldRelation &relation : layout.relations)
          if (relation.linked && canHoldEnd(archetype, relation))
            func::CallOp::create(rewriter, loc, dropFunctionName(relation),
                                 TypeRange{},
                                 ValueRange{id, world.getArena()});
        world.freeEntity(loc, id);
      };
      if (!action) {
        // Only despawns are ever listed for this archetype.
        despawnEntity();
      } else {
        auto despawn = scf::IfOp::create(rewriter, loc, is(0));
        OpBuilder::InsertionGuard inner(rewriter);
        rewriter.setInsertionPointToStart(despawn.thenBlock());
        despawnEntity();
      }
    }
    for (const WorldMove &move : archetype.moves) {
      auto moves = scf::IfOp::create(rewriter, loc, is(move.code));
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(moves.thenBlock());
      applyMove(rewriter, loc, layout, archetype, move, world, row, slot, id,
                tick);
    }
    // Swap-remove the row.
    Value last = arith::SubIOp::create(rewriter, loc,
                                       world.count(loc, archetype), one);
    Value shifts = arith::CmpIOp::create(rewriter, loc,
                                         arith::CmpIPredicate::ne, row, last);
    auto shift = scf::IfOp::create(rewriter, loc, shifts);
    {
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(shift.thenBlock());
      for (const WorldColumn &column : archetype.columns) {
        Value view = column.isStamp()
                         ? world.stamps(archetype, column)
                         : world.column(archetype, column.component,
                                        column.field);
        Value value =
            memref::LoadOp::create(rewriter, loc, view, ValueRange{last});
        memref::StoreOp::create(rewriter, loc, value, view, ValueRange{row});
      }
      Value moved = memref::LoadOp::create(rewriter, loc, world.ids(archetype),
                                           ValueRange{last});
      memref::StoreOp::create(rewriter, loc, moved, world.ids(archetype),
                              ValueRange{row});
      world.setLocation(loc, world.idSlot(loc, moved), archetype, row);
    }
    world.setCount(loc, archetype, last);
  }
  rewriter.setInsertionPointAfter(loop);
  memref::StoreOp::create(rewriter, loc,
                          arith::ConstantIntOp::create(rewriter, loc, 0, 64),
                          counter, ValueRange{zero});
}

/// Lower every ent.spawn in `func`: check the capacity, write the values
/// into the next free row (optional components absent), allocate the id,
/// bump the count.
static SmallVector<Value>
emitPartWith(IRRewriter &rewriter, Location loc, StringRef name,
             TypeRange types, function_ref<SmallVector<Value>()> emit);
static void emitPart(IRRewriter &rewriter, Location loc, const Twine &name,
                     function_ref<void()> emit);

static void lowerSpawns(IRRewriter &rewriter, func::FuncOp func,
                        const WorldLayout &layout, WorldAccess &world) {
  SmallVector<SpawnOp> spawns;
  func.walk([&](SpawnOp spawn) { spawns.push_back(spawn); });
  for (SpawnOp spawn : spawns) {
    const WorldArchetype *archetype = nullptr;
    for (const WorldArchetype &entry : layout.archetypes)
      if (ArchetypeOp(entry.op).getSymNameAttr() ==
          spawn.getArchetypeAttr().getAttr())
        archetype = &entry;
    Location loc = spawn.getLoc();
    rewriter.setInsertionPoint(spawn);
    // (A part: a program that makes its world entity by entity has as
    // many of these as entities, alike but for their values.)
    Value made = emitPartWith(rewriter, loc, "spawn", {world.idType()},
                              [&]() -> SmallVector<Value> {
    // A new entity is out of a sorted archetype's order until the next
    // sort.
    if (archetype->isSorted())
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
          world.edgesClean(layout.getRelation(archetype->sortedBy)),
          ValueRange{arith::ConstantIndexOp::create(rewriter, loc, 0)});
    Value row = world.count(loc, *archetype);
    Value capacity =
        arith::ConstantIndexOp::create(rewriter, loc, archetype->capacity);
    Value fits = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                       row, capacity);
    cf::AssertOp::create(
        rewriter, loc, fits,
        rewriter.getStringAttr("ent.spawn exceeds the capacity of @" +
                               spawn.getArchetypeAttr().getValue()));
    ArchetypeOp archetypeOp = archetype->op;
    // Where each component the entity starts with finds its values: in the
    // order the spawn lists them, or the archetype holds them.
    SmallVector<Attribute> started;
    if (ArrayAttr listed = spawn.getComponentsAttr())
      started.assign(listed.begin(), listed.end());
    else
      for (Attribute attr : archetypeOp.getComponents())
        if (!archetypeOp.isOptional(cast<FlatSymbolRefAttr>(attr)))
          started.push_back(attr);
    llvm::DenseMap<Attribute, unsigned> firstValue;
    unsigned nextValue = 0;
    for (Attribute attr : started) {
      firstValue[attr] = nextValue;
      nextValue += SymbolTable::lookupNearestSymbolFrom<ComponentOp>(
                       spawn, cast<FlatSymbolRefAttr>(attr))
                       .getFieldNames()
                       .size();
    }
    // Logs to append the new entity to, once it has an id.
    SmallVector<std::pair<const WorldLog *, Value>> spawned;
    for (const WorldColumn &column : archetype->columns) {
      auto component = FlatSymbolRefAttr::get(column.component);
      bool starts = firstValue.count(component);
      if (column.isStamp()) {
        // A new entity has added and changed every component it starts
        // with, and lost none.
        bool happened = column.stamp->kind != Trigger::Removed && starts;
        Value stamped =
            happened ? world.currentTick(loc)
                     : arith::ConstantIntOp::create(rewriter, loc, 0, 64)
                           .getResult();
        memref::StoreOp::create(rewriter, loc, stamped,
                                world.stamps(*archetype, column),
                                ValueRange{row});
        if (happened)
          if (const WorldLog *log = layout.findLog(*column.stamp))
            spawned.push_back({log, stamped});
        continue;
      }
      Value stored;
      if (column.isPresence()) {
        stored = arith::ConstantIntOp::create(rewriter, loc, starts, 8);
      } else if (starts) {
        auto componentOp =
            SymbolTable::lookupNearestSymbolFrom<ComponentOp>(spawn, component);
        unsigned field = llvm::find(componentOp.getFieldNames(),
                                    column.field) -
                         componentOp.getFieldNames().begin();
        stored = world.toStorage(
            loc, spawn.getValues()[firstValue[component] + field]);
      } else {
        continue; // an absent optional component's fields stay as they are
      }
      memref::StoreOp::create(
          rewriter, loc, stored,
          world.column(*archetype, column.component, column.field),
          ValueRange{row});
    }
    Value id = world.allocateEntity(loc, *archetype, row);
    if (world.hasIds())
      memref::StoreOp::create(rewriter, loc, id, world.ids(*archetype),
                              ValueRange{row});
    // (Each a part of its own: a new entity is told to as many logs as
    // queries react to what it has, all the same way.)
    for (auto [log, stamped] : spawned)
      emitPart(rewriter, loc, "told", [&, log = log, stamped = stamped] {
        appendToLog(rewriter, loc, world, *log,
                    segmentOf(rewriter, loc, world, *log, row, Value()), id,
                    stamped, Value(), Value(), /*atomic=*/false);
      });
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    world.setCount(loc, *archetype,
                   arith::AddIOp::create(rewriter, loc, row, one));
    return {id};
                              })[0];
    rewriter.replaceOp(spawn, world.fromStorage(loc, made, spawn.getType()));
  }
}

/// The number of entity slots ever used, as an index: a slot id at or above
/// it was never handed out.
static Value loadSlotsInUse(IRRewriter &rewriter, Location loc,
                            const WorldLayout &layout, WorldAccess &world) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  return world.toIndex(
      loc, memref::LoadOp::create(rewriter, loc,
                                  world.scalar(layout.nextSlotOffset),
                                  ValueRange{zero}));
}

/// Emit, at the insertion point, code that finds the entity `id` (in its
/// stored form) among the `candidate` archetypes, and return the values it
/// yields. How the entity is found depends on the entity scheme:
/// a Rows id is its archetype and row (alive if the row is below the
/// archetype's count); a slot id is looked up in the entity table (alive if
/// the slot is in use and, for generational ids, its generation current).
/// `found` is called in the branch where the entity lives at `row` of
/// `archetype`, with the presence of `presenceOf` there (null where the
/// archetype always has it, or `presenceOf` is null); `missing` in every
/// other branch. Both return
/// values of `results` to yield. Every load is guarded, so this is safe for
/// any id.
///
/// A Rows id's row is checked against its archetype's count, a slot id's
/// slot against the number of slots in use. Both are loaded where they are
/// needed, unless `bounds` provides them: a caller that knows they cannot
/// change can load them once, outside a loop.
///
/// With `trusted`, the caller knows the entity is alive, lives in a
/// candidate archetype and has `presenceOf` (an edge's end, see
/// getTrustedEndpoint): `found` is called without any check, for the one
/// candidate directly or in a branch per candidate, the last of which
/// needs no test, and nothing is missing.
static SmallVector<Value>
emitLocate(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
           WorldAccess &world, Value id,
           function_ref<bool(const WorldArchetype &)> candidate,
           FlatSymbolRefAttr presenceOf, TypeRange results,
           function_ref<SmallVector<Value>(const WorldArchetype &archetype,
                                           Value row, Value present)>
               found,
           function_ref<SmallVector<Value>()> missing,
           const LocateBounds &bounds, bool trusted, Value location) {
  const EntityScheme &scheme = layout.entities;
  // No archetype is a candidate (a component no entity of the program
  // has, say): the entity is nowhere to be found.
  if (llvm::none_of(layout.archetypes, candidate))
    return missing();
  // (`location`: the trusted entity's packed location, where the caller
  // has it and the entity table need not be asked.)
  auto whereAndRow = [&]() -> std::pair<Value, Value> {
    if (location)
      return world.unpackLocation(loc, location);
    return scheme.kind == EntityScheme::Rows
               ? world.unpackRows(loc, id)
               : world.getLocation(loc, world.idSlot(loc, id));
  };
  if (trusted) {
    const WorldArchetype *only = nullptr;
    unsigned candidates = 0;
    for (const WorldArchetype &archetype : layout.archetypes)
      if (candidate(archetype)) {
        only = &archetype;
        ++candidates;
      }
    if (candidates == 1)
      return found(*only, whereAndRow().second, Value());
    if (candidates > 1) {
      auto [where, row] = whereAndRow();
      unsigned bits = cast<IntegerType>(where.getType()).getWidth();
      OpBuilder::InsertionGuard guard(rewriter);
      scf::IfOp top;
      unsigned left = candidates;
      for (const WorldArchetype &archetype : layout.archetypes) {
        if (!candidate(archetype))
          continue;
        if (--left == 0) {
          SmallVector<Value> values = found(archetype, row, Value());
          if (!results.empty())
            scf::YieldOp::create(rewriter, loc, values);
          break;
        }
        Value here = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq, where,
            arith::ConstantIntOp::create(rewriter, loc, archetype.index,
                                         bits));
        auto branch = scf::IfOp::create(rewriter, loc, results, here,
                                        /*withElseRegion=*/true);
        if (top && !results.empty())
          scf::YieldOp::create(rewriter, loc, branch.getResults());
        if (!top)
          top = branch;
        rewriter.setInsertionPointToStart(branch.thenBlock());
        SmallVector<Value> values = found(archetype, row, Value());
        if (!results.empty())
          scf::YieldOp::create(rewriter, loc, values);
        rewriter.setInsertionPointToStart(branch.elseBlock());
      }
      return SmallVector<Value>(top.getResults());
    }
  }
  // Without results, scf.if blocks come with their terminator.
  auto yield = [&](ValueRange values) {
    if (!results.empty())
      scf::YieldOp::create(rewriter, loc, values);
  };
  // An `if` with `missing` in its else branch; leaves the insertion point
  // in the then branch and returns the op.
  auto guard = [&](Value condition) {
    auto branch = scf::IfOp::create(rewriter, loc, results, condition,
                                    /*withElseRegion=*/true);
    rewriter.setInsertionPointToStart(branch.elseBlock());
    yield(missing());
    rewriter.setInsertionPointToStart(branch.thenBlock());
    return branch;
  };

  Value where, row;
  OpBuilder::InsertionGuard outer(rewriter);
  OpBuilder::InsertPoint start = rewriter.saveInsertionPoint();
  scf::IfOp top;
  if (scheme.kind == EntityScheme::Rows) {
    std::tie(where, row) = world.unpackRows(loc, id);
  } else {
    Value slot = world.idSlot(loc, id);
    Value used = bounds.slotsInUse ? bounds.slotsInUse
                                   : loadSlotsInUse(rewriter, loc, layout,
                                                    world);
    top = guard(arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                      slot, used));
    if (scheme.hasGenerations()) {
      Value current = memref::LoadOp::create(rewriter, loc, world.generations(),
                                             ValueRange{slot});
      Value alive =
          arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                current, world.idGeneration(loc, id));
      auto checkAlive = guard(alive);
      rewriter.setInsertionPointToEnd(top.thenBlock());
      if (!results.empty())
        yield(checkAlive.getResults());
      rewriter.setInsertionPointToStart(checkAlive.thenBlock());
    }
    std::tie(where, row) = world.getLocation(loc, slot);
  }

  // One branch per archetype that holds the component; the innermost else
  // finds nothing.
  unsigned whereBits = cast<IntegerType>(where.getType()).getWidth();
  bool any = false;
  for (const WorldArchetype &archetype : layout.archetypes) {
    ArchetypeOp archetypeOp = archetype.op;
    if (!candidate(archetype))
      continue;
    Value here = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq, where,
        arith::ConstantIntOp::create(rewriter, loc, archetype.index,
                                     whereBits));
    auto branch = scf::IfOp::create(rewriter, loc, results, here,
                                    /*withElseRegion=*/true);
    if (!any && !top)
      top = branch;
    else
      yield(branch.getResults());
    any = true;
    rewriter.setInsertionPointToStart(branch.thenBlock());
    Value inRow = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
    if (scheme.kind == EntityScheme::Rows)
      inRow = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::ult, row,
          bounds.counts.empty() ? world.count(loc, archetype)
                                : bounds.counts[archetype.index]);
    auto inside = guard(inRow);
    Value present;
    if (presenceOf && archetypeOp.isOptional(presenceOf)) {
      Value byte = memref::LoadOp::create(
          rewriter, loc,
          world.column(archetype, presenceOf.getAttr(),
                       rewriter.getStringAttr("")),
          ValueRange{row});
      present = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::ne, byte,
          arith::ConstantIntOp::create(rewriter, loc, 0, 8));
    }
    yield(found(archetype, row, present));
    rewriter.setInsertionPointAfter(inside);
    yield(inside.getResults());
    rewriter.setInsertionPointToStart(branch.elseBlock());
  }
  if (top) {
    yield(missing());
  } else {
    // Rows ids and no archetype holds the component: nothing is found.
    rewriter.restoreInsertionPoint(start);
    top = guard(arith::ConstantIntOp::create(rewriter, loc, 0, 1));
  }
  return SmallVector<Value>(top.getResults());
}

/// Lower every ent.lookup in `func` into guarded loads (see emitLocate).
/// Where the component is optional, `found` is its presence.
namespace {
/// An entity found without a branch for each archetype it may be in:
/// whether it is alive and in one of `homes`, which of them it is in
/// (`isHere`, one for each), and its row (0 where it is not found). What
/// is read of it comes from where a chain of selects says its column
/// starts in the world's memory, by one load: for programs with many
/// archetypes, where a branch for each is most of the code.
struct TableLocation {
  SmallVector<const WorldArchetype *> homes;
  SmallVector<Value> isHere;
  Value found, row, rows;
};
} // namespace

/// Find the entity `id` (in its stored form) among `homes`, at the
/// insertion point. With `trusted` it is known to be alive and in one.
static TableLocation
locateByTable(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
              WorldAccess &world, Value id,
              ArrayRef<const WorldArchetype *> homes, bool trusted) {
  const EntityScheme &scheme = layout.entities;
  TableLocation at;
  at.homes.assign(homes.begin(), homes.end());
  Type index = rewriter.getIndexType();
  Value yes = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
  auto both = [&](Value a, Value b) -> Value {
    return arith::AndIOp::create(rewriter, loc, a, b);
  };
  bool byRows = scheme.kind == EntityScheme::Rows;
  Value slot = byRows ? Value() : world.idSlot(loc, id);
  Value alive = yes;
  if (!trusted && !byRows) {
    alive = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                  slot,
                                  loadSlotsInUse(rewriter, loc, layout, world));
    // (A slot that was never used has no generation and no location to
    // ask for: the first one's are asked in its place.)
    slot = arith::SelectOp::create(
        rewriter, loc, alive, slot,
        arith::ConstantIndexOp::create(rewriter, loc, 0));
    if (scheme.hasGenerations())
      alive = both(alive,
                   arith::CmpIOp::create(
                       rewriter, loc, arith::CmpIPredicate::eq,
                       memref::LoadOp::create(rewriter, loc,
                                              world.generations(),
                                              ValueRange{slot}),
                       world.idGeneration(loc, id)));
  }
  auto [where, row] =
      byRows ? world.unpackRows(loc, id) : world.getLocation(loc, slot);
  unsigned bits = cast<IntegerType>(where.getType()).getWidth();
  Value there = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
  for (const WorldArchetype *home : homes) {
    at.isHere.push_back(arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq, where,
        arith::ConstantIntOp::create(rewriter, loc, home->index, bits)));
    there = arith::OrIOp::create(rewriter, loc, there, at.isHere.back());
  }
  // (An id that is a row is of an entity if its archetype has so many.)
  if (!trusted && byRows) {
    Value which = arith::SelectOp::create(
        rewriter, loc, there,
        arith::IndexCastUIOp::create(rewriter, loc, index, where),
        arith::ConstantIndexOp::create(rewriter, loc, homes[0]->index));
    Value count = arith::IndexCastOp::create(
        rewriter, loc, index,
        memref::LoadOp::create(rewriter, loc, world.counts(),
                               ValueRange{which}));
    alive = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                  row, count);
  }
  at.found = both(alive, there);
  // (Where it is not there, the first row of the first is read, to no
  // effect.)
  at.row = arith::SelectOp::create(
      rewriter, loc, at.found, row,
      arith::ConstantIndexOp::create(rewriter, loc, 0));
  at.rows = arith::AddIOp::create(
      rewriter, loc, at.row, arith::ConstantIndexOp::create(rewriter, loc, 1));
  return at;
}

/// What the entity at `at` has in the column that starts, for each of its
/// homes, at the offset `column` gives.
static Value
readByTable(IRRewriter &rewriter, Location loc, WorldAccess &world,
            const TableLocation &at, Type type,
            function_ref<uint64_t(const WorldArchetype &)> column) {
  Value offset =
      arith::ConstantIndexOp::create(rewriter, loc, column(*at.homes[0]));
  for (unsigned k = 1; k < at.homes.size(); ++k)
    offset = arith::SelectOp::create(
        rewriter, loc, at.isHere[k],
        arith::ConstantIndexOp::create(rewriter, loc, column(*at.homes[k])),
        offset);
  Value view = memref::ViewOp::create(
      rewriter, loc, MemRefType::get({ShapedType::kDynamic}, type),
      world.getArena(), offset, ValueRange{at.rows});
  return memref::LoadOp::create(rewriter, loc, view, ValueRange{at.row})
      .getResult();
}

/// Read fields of `component` of the entity `id` (in its stored form):
/// their values (stored form), then whether the entity is alive and has
/// the component (zeros where it is not or has not). As emitLocate with a
/// load of each field where the entity is found, by a table (see
/// TableLocation). Null where that does not pay: few archetypes.
static std::optional<SmallVector<Value>>
emitReadFields(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
               WorldAccess &world, Value id, FlatSymbolRefAttr component,
               ArrayRef<StringAttr> fields, bool trusted) {
  SmallVector<const WorldArchetype *> homes;
  for (const WorldArchetype &archetype : layout.archetypes)
    if (ArchetypeOp(archetype.op).contains(component))
      homes.push_back(&archetype);
  if (homes.size() < 3)
    return std::nullopt;
  TableLocation at =
      locateByTable(rewriter, loc, layout, world, id, homes, trusted);
  SmallVector<Value> values;
  for (StringAttr field : fields) {
    Type type =
        world.storageType(homes[0]->find(component.getAttr(), field)->type);
    Value value = readByTable(rewriter, loc, world, at, type,
                              [&](const WorldArchetype &home) {
                                return home.find(component.getAttr(), field)
                                    ->offset;
                              });
    if (!trusted)
      value = arith::SelectOp::create(
          rewriter, loc, at.found, value,
          arith::ConstantOp::create(
              rewriter, loc, cast<TypedAttr>(rewriter.getZeroAttr(type))));
    values.push_back(value);
  }
  // Present: in an archetype that holds the component optionally, the
  // entity's byte says.
  Value present = at.found;
  StringAttr none = rewriter.getStringAttr("");
  auto optionally = [&](const WorldArchetype *home) {
    return ArchetypeOp(home->op).isOptional(component);
  };
  if (llvm::any_of(homes, optionally)) {
    // (An archetype that always has it has no such byte: one that is
    // there is read, and not asked.)
    const WorldArchetype *withByte = *llvm::find_if(homes, optionally);
    Value byte = readByTable(
        rewriter, loc, world, at, rewriter.getI8Type(),
        [&](const WorldArchetype &home) {
          return (optionally(&home) ? home : *withByte)
              .find(component.getAttr(), none)
              ->offset;
        });
    Value optional = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
    for (auto [k, home] : llvm::enumerate(homes))
      if (optionally(home))
        optional = arith::OrIOp::create(rewriter, loc, optional, at.isHere[k]);
    Value has = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::ne, byte,
        arith::ConstantIntOp::create(rewriter, loc, 0, 8));
    present = arith::AndIOp::create(
        rewriter, loc, present,
        arith::SelectOp::create(
            rewriter, loc, optional, has,
            arith::ConstantIntOp::create(rewriter, loc, 1, 1)));
  }
  values.push_back(present);
  return values;
}

/// The tick `stamp` has for the entity `id`, 0 if it has none or is no
/// more, the same way. Null where that does not pay.
static std::optional<Value>
emitReadStamp(IRRewriter &rewriter, Location loc, const WorldLayout &layout,
              WorldAccess &world, Value id, const Stamp &stamp,
              bool trusted) {
  SmallVector<const WorldArchetype *> homes;
  for (const WorldArchetype &archetype : layout.archetypes)
    if (archetype.findStamp(stamp))
      homes.push_back(&archetype);
  if (homes.size() < 3)
    return std::nullopt;
  TableLocation at =
      locateByTable(rewriter, loc, layout, world, id, homes, trusted);
  Type i64 = rewriter.getI64Type();
  Value stamped = readByTable(rewriter, loc, world, at, i64,
                              [&](const WorldArchetype &home) {
                                return home.findStamp(stamp)->offset;
                              });
  return arith::SelectOp::create(
             rewriter, loc, at.found, stamped,
             arith::ConstantIntOp::create(rewriter, loc, 0, 64))
      .getResult();
}

static void lowerLookups(IRRewriter &rewriter, func::FuncOp func,
                         const WorldLayout &layout, WorldAccess &world) {
  SmallVector<LookupOp> lookups;
  func.walk([&](LookupOp lookup) { lookups.push_back(lookup); });
  if (lookups.empty())
    return;
  // Lookups of several fields of one component of one entity find the
  // entity once, where the first of them stands, and read all the fields
  // there: finding it is most of what a lookup costs, in time and in
  // code. Only where that one is before the others whatever happens, and
  // nothing between could have written the component (nothing does in
  // the loop they are in).
  auto scopeOf = [&](Operation *op) {
    while (op->getParentOp() != func.getOperation())
      op = op->getParentOp();
    return op;
  };
  llvm::DenseMap<std::pair<Operation *, Attribute>, bool> written;
  auto writes = [&](Operation *scope, FlatSymbolRefAttr component) {
    auto [entry, isNew] = written.insert({{scope, component}, false});
    if (!isNew)
      return entry->second;
    llvm::DenseSet<Value> columns;
    for (const WorldArchetype &archetype : layout.archetypes)
      for (const WorldColumn &column : archetype.columns)
        if (column.component == component.getAttr() && !column.isStamp())
          columns.insert(
              world.column(archetype, column.component, column.field));
    bool any = false;
    scope->walk([&](memref::StoreOp store) {
      any |= columns.contains(store.getMemRef());
    });
    // (Or what is not lowered yet may: a combine into another entity.)
    scope->walk([&](Operation *op) {
      if (auto apply = dyn_cast<ApplyOp>(op))
        any |= apply.getComponentAttr() == component;
    });
    return written[{scope, component}] = any;
  };
  DominanceInfo dominance(func);
  SmallVector<SmallVector<LookupOp, 4>> groups;
  for (LookupOp lookup : lookups) {
    bool placed = false;
    if (!writes(scopeOf(lookup), lookup.getComponentAttr()))
      for (auto &group : groups) {
        LookupOp first = group.front();
        if (first.getEntity() == lookup.getEntity() &&
            first.getComponentAttr() == lookup.getComponentAttr() &&
            first->hasAttr(kTrustedAttr) == lookup->hasAttr(kTrustedAttr) &&
            dominance.properlyDominates(first.getOperation(),
                                        lookup.getOperation())) {
          group.push_back(lookup);
          placed = true;
          break;
        }
      }
    if (!placed)
      groups.push_back({lookup});
  }
  for (auto &group : groups) {
    LookupOp first = group.front();
    Location loc = first.getLoc();
    rewriter.setInsertionPoint(first);
    FlatSymbolRefAttr component = first.getComponentAttr();
    SmallVector<StringAttr> fields;
    SmallVector<Type> resultTypes;
    for (LookupOp lookup : group)
      if (!llvm::is_contained(fields, lookup.getFieldAttr())) {
        fields.push_back(lookup.getFieldAttr());
        resultTypes.push_back(world.storageType(lookup.getValue().getType()));
      }
    resultTypes.push_back(rewriter.getI1Type());
    auto holds = [&](const WorldArchetype &archetype) {
      return ArchetypeOp(archetype.op).contains(component);
    };
    std::optional<SmallVector<Value>> byTable = emitReadFields(
        rewriter, loc, layout, world, world.toStorage(loc, first.getEntity()),
        component, fields, first->hasAttr(kTrustedAttr));
    SmallVector<Value> results = byTable ? *byTable : emitLocate(
        rewriter, loc, layout, world, world.toStorage(loc, first.getEntity()),
        holds, component, resultTypes,
        [&](const WorldArchetype &archetype, Value row,
            Value present) -> SmallVector<Value> {
          SmallVector<Value> read;
          for (StringAttr field : fields)
            read.push_back(memref::LoadOp::create(
                rewriter, loc,
                world.column(archetype, component.getAttr(), field),
                ValueRange{row}));
          if (!present)
            present = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
          read.push_back(present);
          return read;
        },
        [&]() -> SmallVector<Value> {
          SmallVector<Value> nothing;
          for (Type type : ArrayRef<Type>(resultTypes).drop_back())
            nothing.push_back(arith::ConstantOp::create(
                rewriter, loc, cast<TypedAttr>(rewriter.getZeroAttr(type))));
          nothing.push_back(arith::ConstantIntOp::create(rewriter, loc, 0, 1));
          return nothing;
        },
        LocateBounds(), first->hasAttr(kTrustedAttr));
    SmallVector<Value> converted;
    for (LookupOp lookup : group)
      converted.push_back(world.fromStorage(
          loc, results[llvm::find(fields, lookup.getFieldAttr()) -
                       fields.begin()],
          lookup.getValue().getType()));
    for (auto [lookup, value] : llvm::zip(group, converted))
      rewriter.replaceOp(lookup, {value, results.back()});
  }
}

/// `a ⊕ b` under an apply's rule; min and max are signed for integers and
/// IEEE-754 minimum/maximum (NaN-propagating) for floats.
static Value combine(IRRewriter &rewriter, Location loc, StringRef rule,
                     Value a, Value b) {
  bool isFloat = isa<FloatType>(a.getType());
  if (rule == "add")
    return isFloat ? arith::AddFOp::create(rewriter, loc, a, b).getResult()
                   : arith::AddIOp::create(rewriter, loc, a, b).getResult();
  if (rule == "min")
    return isFloat ? arith::MinimumFOp::create(rewriter, loc, a, b).getResult()
                   : arith::MinSIOp::create(rewriter, loc, a, b).getResult();
  assert(rule == "max" && "the verifier admits add, min and max");
  return isFloat ? arith::MaximumFOp::create(rewriter, loc, a, b).getResult()
                 : arith::MaxSIOp::create(rewriter, loc, a, b).getResult();
}

/// The bounds the ids an apply sends to are checked against, loaded once:
/// combining only writes fields, so they cannot change meanwhile. Loaded
/// inside the loop, they would be reloaded for every value: LLVM cannot
/// tell the stores to the field from them, since all live in the arena.
static LocateBounds loadApplyBounds(IRRewriter &rewriter, Location loc,
                                    ApplyOp apply, const WorldLayout &layout,
                                    WorldAccess &world) {
  LocateBounds bounds;
  if (layout.entities.kind == EntityScheme::Rows) {
    for (const WorldArchetype &target : layout.archetypes) {
      ArchetypeOp targetOp = target.op;
      bounds.counts.push_back(targetOp.contains(apply.getComponentAttr())
                                  ? world.count(loc, target)
                                  : Value());
    }
  } else {
    bounds.slotsInUse = loadSlotsInUse(rewriter, loc, layout, world);
  }
  return bounds;
}

/// Combine `value` (stored form) into `apply`'s field of the entity `id`,
/// at the insertion point; nothing if the id is not alive or the entity
/// lacks the component.
static void combineInto(IRRewriter &rewriter, ApplyOp apply,
                        const WorldLayout &layout, WorldAccess &world,
                        Value id, Value value, Value tick,
                        const LocateBounds &bounds) {
  Location loc = apply.getLoc();
  FlatSymbolRefAttr component = apply.getComponentAttr();
  auto holds = [&](const WorldArchetype &archetype) {
    return ArchetypeOp(archetype.op).contains(component);
  };
  emitLocate(
      rewriter, loc, layout, world, id, holds, component, TypeRange{},
      [&](const WorldArchetype &target, Value targetRow,
          Value present) -> SmallVector<Value> {
        OpBuilder::InsertionGuard inner(rewriter);
        if (present) {
          auto ifPresent = scf::IfOp::create(rewriter, loc, present);
          rewriter.setInsertionPointToStart(ifPresent.thenBlock());
        }
        combineAtRow(rewriter, loc, layout, world, target, targetRow,
                     component.getAttr(), apply.getFieldAttr(),
                     apply.getRule(), value, id, tick);
        return {};
      },
      []() -> SmallVector<Value> { return {}; }, bounds,
      apply->hasAttr(kTrustedAttr));
}

static void combineAtRow(IRRewriter &rewriter, Location loc,
                         const WorldLayout &layout, WorldAccess &world,
                         const WorldArchetype &target, Value row,
                         StringAttr component, StringAttr fieldName,
                         StringRef rule, Value value, Value id, Value tick) {
  Value field = world.column(target, component, fieldName);
  Value old = memref::LoadOp::create(rewriter, loc, field, ValueRange{row});
  Value combined = combine(rewriter, loc, rule, old, value);
  memref::StoreOp::create(rewriter, loc, combined, field, ValueRange{row});
  // (An event only where the field has another value for it.)
  Value differs =
      stampsFor(target, Trigger::Changed, component, fieldName).empty()
          ? Value()
          : emitDiffers(rewriter, loc, old, combined);
  // What a tree's children are ordered by: the tree is looked over when
  // the query ends (see noteOrderWrites).
  for (const WorldRelation &relation : layout.relations)
    if (relation.orderComponent == component &&
        relation.orderField == fieldName)
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
          world.edgesClean(relation),
          ValueRange{
              arith::ConstantIndexOp::create(rewriter, loc, 0).getResult()});
  for (const WorldColumn *column :
       stampsFor(target, Trigger::Changed, component, fieldName)) {
    Value stamps = world.stamps(target, *column);
    const WorldLog *log = layout.findLog(*column->stamp);
    Value before = log || differs
                       ? memref::LoadOp::create(rewriter, loc, stamps,
                                                ValueRange{row})
                             .getResult()
                       : Value();
    memref::StoreOp::create(
        rewriter, loc,
        differs ? arith::SelectOp::create(rewriter, loc, differs, tick, before)
                      .getResult()
                : tick,
        stamps, ValueRange{row});
    if (log)
      appendToLog(rewriter, loc, world, *log,
                  segmentOf(rewriter, loc, world, *log, row, Value()),
                  id ? id : world.entityId(loc, target, row), tick, before,
                  differs, /*atomic=*/false);
  }
}

/// Combine the values `apply` sent from the first `count` rows of
/// `archetype` into their targets, at the insertion point, one row after
/// another: the order is fixed, so the result does not depend on how the
/// query's loop ran. Rows that sent nothing, dead ids and targets without
/// the component are skipped.
static void combineApplied(IRRewriter &rewriter, ApplyOp apply,
                           const WorldLayout &layout,
                           const WorldArchetype &archetype, WorldAccess &world,
                           Value count, Value tick) {
  Location loc = apply.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  auto [ids, values] = world.applyBuffer(apply, archetype);
  LocateBounds bounds = loadApplyBounds(rewriter, loc, apply, layout, world);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
  rewriter.setInsertionPoint(loop.getBody()->getTerminator());
  Value row = loop.getInductionVar();
  Value id = memref::LoadOp::create(rewriter, loc, ids, ValueRange{row});
  Value sent = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                     id, world.noEntity(loc));
  auto ifSent = scf::IfOp::create(rewriter, loc, sent);
  rewriter.setInsertionPointToStart(ifSent.thenBlock());
  Value value = memref::LoadOp::create(rewriter, loc, values, ValueRange{row});
  combineInto(rewriter, apply, layout, world, id, value, tick, bounds);
}

static void combineDirectly(IRRewriter &rewriter, ApplyOp apply,
                            const WorldLayout &layout, WorldAccess &world,
                            Value id, Value value, Value tick) {
  OpBuilder::InsertionGuard guard(rewriter);
  combineInto(rewriter, apply, layout, world, id, value, tick, LocateBounds());
}

/// The positions of the edges of the entity with key `key` (an index) in
/// the order `in` or `out` visits them: [begin, end), as indices.
static std::pair<Value, Value> emitEdgeRange(IRRewriter &rewriter,
                                             Location loc, WorldAccess &world,
                                             const WorldRelation &relation,
                                             bool in, Value key) {
  Value offsets = world.edgeOffsets(relation, in);
  Value next = arith::AddIOp::create(
      rewriter, loc, key, arith::ConstantIndexOp::create(rewriter, loc, 1));
  Value begin = memref::LoadOp::create(rewriter, loc, offsets, ValueRange{key});
  Value end = memref::LoadOp::create(rewriter, loc, offsets, ValueRange{next});
  return {world.toIndex(loc, begin), world.toIndex(loc, end)};
}

/// The parent of the entity `id` (in its stored form) in the tree
/// `relation`, at the insertion point: whether it has one, and the
/// parent's id (no entity otherwise). The parent is the target of the
/// entity's one edge; with generational ids a slot may have been reused
/// since the edges were sorted, so the edge must be the entity's own.
static std::pair<Value, Value> emitParent(IRRewriter &rewriter, Location loc,
                                          const WorldLayout &layout,
                                          WorldAccess &world,
                                          const WorldRelation &relation,
                                          Value id) {
  // A linked tree: the slot of the entity's key, if it holds its id.
  if (relation.linked) {
    Value key = world.entityKey(loc, id);
    Value mine = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq,
        memref::LoadOp::create(rewriter, loc,
                               world.edgeIds(relation, /*source=*/true),
                               ValueRange{key}),
        world.slotOwner(loc, id));
    Value parent = memref::LoadOp::create(
        rewriter, loc, world.edgeIds(relation, /*source=*/false),
        ValueRange{key});
    return {mine, parent};
  }
  auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                    /*in=*/false, world.entityKey(loc, id));
  Value any = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                    begin, end);
  bool sourceTrusted =
      relation.getTrusted(/*target=*/false) != FlatSymbolRefAttr();
  auto branch = scf::IfOp::create(
      rewriter, loc, TypeRange{rewriter.getI1Type(), world.idType()}, any,
      /*withElseRegion=*/true);
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToStart(branch.thenBlock());
  Value mine = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
  if (layout.entities.hasGenerations() && !sourceTrusted)
    mine = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq,
        memref::LoadOp::create(rewriter, loc,
                               world.edgeIds(relation, /*source=*/true),
                               ValueRange{begin}),
        id);
  Value parent = memref::LoadOp::create(
      rewriter, loc, world.edgeIds(relation, /*source=*/false),
      ValueRange{begin});
  scf::YieldOp::create(rewriter, loc, ValueRange{mine, parent});
  rewriter.setInsertionPointToStart(branch.elseBlock());
  scf::YieldOp::create(
      rewriter, loc,
      ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 1),
                 world.noEntity(loc)});
  return {branch.getResult(0), branch.getResult(1)};
}

/// The nearest ancestor of the entity `id` (in its stored form) along the
/// tree `relation` that has `component`, at the insertion point: its
/// parent if that has the component, else the parent's parent, and so on,
/// as long as the entities on the way are alive. Where the relation's
/// targets are trusted to have the component, that is the parent. A caller
/// that has the entity's parent at hand gives it as `parent`; `found` is
/// then null where the parent is the ancestor without a doubt. With
/// `direct` it is the parent or nothing: no further ancestor is asked.
static Ancestor emitAncestor(IRRewriter &rewriter, Location loc,
                             const WorldLayout &layout, WorldAccess &world,
                             const WorldRelation &relation,
                             FlatSymbolRefAttr component, Value id,
                             Value parent, bool direct, unsigned hops) {
  Type i1 = rewriter.getI1Type();
  Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
  Value yes = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
  bool trusted = relation.getTrusted(/*target=*/true) == component;
  if (trusted && parent)
    return {Value(), parent, /*trusted=*/true};
  Value has = yes;
  if (!parent)
    std::tie(has, parent) =
        emitParent(rewriter, loc, layout, world, relation, id);
  // More steps: the parent's parent, and on. (Where there is none, the
  // entity is asked in its place, to no effect.)
  for (unsigned step = 1; step < hops; ++step) {
    auto [more, next] = emitParent(
        rewriter, loc, layout, world, relation,
        arith::SelectOp::create(rewriter, loc, has, parent, id));
    has = arith::AndIOp::create(rewriter, loc, has, more);
    parent = arith::SelectOp::create(rewriter, loc, has, next,
                                     world.noEntity(loc));
  }
  if (trusted)
    return {has, parent, /*trusted=*/true};
  if (direct) {
    if (auto byTable = emitReadFields(rewriter, loc, layout, world, parent,
                                      component, {}, /*trusted=*/false))
      return {arith::AndIOp::create(rewriter, loc, has, byTable->back()),
              parent, /*trusted=*/false};
    Value holds = emitLocate(
        rewriter, loc, layout, world, parent,
        [](const WorldArchetype &) { return true; }, component, TypeRange{i1},
        [&](const WorldArchetype &archetype, Value,
            Value present) -> SmallVector<Value> {
          if (!ArchetypeOp(archetype.op).contains(component))
            return {no};
          return {present ? present : yes};
        },
        [&]() -> SmallVector<Value> { return {no}; })[0];
    return {arith::AndIOp::create(rewriter, loc, has, holds), parent,
            /*trusted=*/false};
  }
  // (the entity to look at, whether it is the ancestor, whether to look)
  auto climb = scf::WhileOp::create(
      rewriter, loc, TypeRange{world.idType(), i1, i1},
      ValueRange{parent, no, has},
      [&](OpBuilder &builder, Location, ValueRange state) {
        scf::ConditionOp::create(builder, loc, state[2], state);
      },
      [&](OpBuilder &, Location, ValueRange state) {
        // Whether it is alive, and whether it has the component.
        SmallVector<Value> located = emitLocate(
            rewriter, loc, layout, world, state[0],
            [](const WorldArchetype &) { return true; }, component,
            TypeRange{i1, i1},
            [&](const WorldArchetype &archetype, Value,
                Value present) -> SmallVector<Value> {
              if (!ArchetypeOp(archetype.op).contains(component))
                return {yes, no};
              return {yes, present ? present : yes};
            },
            [&]() -> SmallVector<Value> { return {no, no}; });
        Value alive = located[0], found = located[1];
        // Not it, but alive: on to its parent, if it has one.
        Value further = arith::AndIOp::create(
            rewriter, loc, alive,
            arith::XOrIOp::create(rewriter, loc, found, yes));
        auto branch = scf::IfOp::create(
            rewriter, loc, TypeRange{world.idType(), i1}, further,
            /*withElseRegion=*/true);
        {
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPointToStart(branch.thenBlock());
          auto [hasNext, next] =
              emitParent(rewriter, loc, layout, world, relation, state[0]);
          scf::YieldOp::create(rewriter, loc, ValueRange{next, hasNext});
          rewriter.setInsertionPointToStart(branch.elseBlock());
          scf::YieldOp::create(rewriter, loc, ValueRange{state[0], no});
        }
        scf::YieldOp::create(
            rewriter, loc,
            ValueRange{branch.getResult(0), found, branch.getResult(1)});
      });
  return {climb.getResult(1), climb.getResult(0), /*trusted=*/false};
}

/// The sibling before the entity at `row` of `archetype` in the tree
/// `relation`, whose children are in an order, at the insertion point:
/// the child of the same parent that comes right before it, if there is
/// one and it has `component`. By the tree's links; in a sorted tree by
/// the row before, or across archetypes by the id the sort left.
static Ancestor emitSibling(IRRewriter &rewriter, Location loc,
                            const WorldLayout &layout, WorldAccess &world,
                            const WorldRelation &relation,
                            FlatSymbolRefAttr component,
                            const WorldArchetype &archetype, Value row,
                            bool after) {
  Type i1 = rewriter.getI1Type();
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value has, sibling;
  if (relation.linked) {
    Value id = world.entityId(loc, archetype, row);
    Value key = world.entityKey(loc, id);
    Value mine = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq,
        memref::LoadOp::create(rewriter, loc,
                               world.edgeIds(relation, /*source=*/true),
                               ValueRange{key}),
        world.slotOwner(loc, id));
    Value link = memref::LoadOp::create(
        rewriter, loc,
        world.treeLinks(relation, after ? relation.nextSiblingOffset
                                        : relation.previousSiblingOffset),
        ValueRange{key});
    has = arith::AndIOp::create(
        rewriter, loc, mine,
        arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::ne, link,
            arith::ConstantIntOp::create(rewriter, loc, 0,
                                         relation.offsetBits)));
    Value slot = arith::SelectOp::create(
        rewriter, loc, has,
        arith::SubIOp::create(rewriter, loc, world.toIndex(loc, link), one),
        zero);
    sibling = arith::SelectOp::create(
        rewriter, loc, has,
        world.ownerId(loc, memref::LoadOp::create(
                               rewriter, loc,
                               world.edgeIds(relation, /*source=*/true),
                               ValueRange{slot})),
        world.noEntity(loc));
  } else if (!llvm::is_contained(relation.sortedArchetypes, archetype.index)) {
    // Not an archetype of the tree: no entity of it has a sibling.
    return {arith::ConstantIntOp::create(rewriter, loc, 0, 1),
            world.noEntity(loc), /*trusted=*/false};
  } else if (relation.sortedArchetype() >= 0) {
    // The rows are in the tree's order: the row before, if it is past
    // the entities without a parent and has the same parent.
    Value roots = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, world.rootCount(archetype),
                                    ValueRange{zero}));
    // (Or the row after, of an entity that has a parent itself.)
    Value next = arith::AddIOp::create(rewriter, loc, row, one);
    Value past =
        after ? arith::AndIOp::create(
                    rewriter, loc,
                    arith::CmpIOp::create(rewriter, loc,
                                          arith::CmpIPredicate::uge, row,
                                          roots),
                    arith::CmpIOp::create(rewriter, loc,
                                          arith::CmpIPredicate::ult, next,
                                          world.count(loc, archetype)))
                    .getResult()
              : arith::CmpIOp::create(rewriter, loc,
                                      arith::CmpIPredicate::ugt, row, roots)
                    .getResult();
    Value before = arith::SelectOp::create(
        rewriter, loc, past,
        after ? next
              : arith::SubIOp::create(rewriter, loc, row, one).getResult(),
        row);
    Value parents = world.parentRows(archetype);
    has = arith::AndIOp::create(
        rewriter, loc, past,
        arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq,
            memref::LoadOp::create(rewriter, loc, parents, ValueRange{before}),
            memref::LoadOp::create(rewriter, loc, parents, ValueRange{row})));
    ArchetypeOp archetypeOp = archetype.op;
    if (archetypeOp.contains(component) && !archetypeOp.isOptional(component))
      return {has, Value(), /*trusted=*/true, {&archetype}, Value(), before};
    sibling = arith::SelectOp::create(
        rewriter, loc, has,
        memref::LoadOp::create(rewriter, loc, world.ids(archetype),
                               ValueRange{before}),
        world.noEntity(loc));
  } else {
    sibling = memref::LoadOp::create(
        rewriter, loc,
        after ? world.afterIds(archetype) : world.beforeIds(archetype),
        ValueRange{row});
    has = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                sibling, world.noEntity(loc));
  }
  // A sibling is a source of the tree's edges: where those all have the
  // component, it does.
  if (relation.getTrusted(/*target=*/false) == component)
    return {has, sibling, /*trusted=*/true};
  Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
  Value yes = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
  Value holds = emitLocate(
      rewriter, loc, layout, world, sibling,
      [](const WorldArchetype &) { return true; }, component, TypeRange{i1},
      [&](const WorldArchetype &archetype, Value,
          Value present) -> SmallVector<Value> {
        if (!ArchetypeOp(archetype.op).contains(component))
          return {no};
        return {present ? present : yes};
      },
      [&]() -> SmallVector<Value> { return {no}; })[0];
  return {arith::AndIOp::create(rewriter, loc, has, holds), sibling,
          /*trusted=*/false};
}

/// A loop over the edges of the entity `id` (in its stored form, with
/// key `key`) in a linked tree, at the insertion point: its one edge out,
/// if its slot holds one, or the edges to it, which are a list through
/// their sources' slots. Returns the loop and the edge's slot (an index),
/// and leaves the insertion point in the loop's body, after the slot.
static std::pair<scf::ForOp, Value>
emitLinkedEdges(IRRewriter &rewriter, Location loc, WorldAccess &world,
                const WorldRelation &relation, bool in, Value key, Value id) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  if (!in) {
    Value mine = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::eq,
        memref::LoadOp::create(rewriter, loc,
                               world.edgeIds(relation, /*source=*/true),
                               ValueRange{key}),
        world.slotOwner(loc, id));
    auto loop = scf::ForOp::create(
        rewriter, loc, zero,
        arith::SelectOp::create(rewriter, loc, mine, one, zero), one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    return {loop, key};
  }
  Value number = world.toIndex(
      loc, memref::LoadOp::create(
               rewriter, loc,
               world.treeLinks(relation, relation.childCountOffset),
               ValueRange{key}));
  Value first = world.toIndex(
      loc, memref::LoadOp::create(
               rewriter, loc,
               world.treeLinks(relation, relation.firstChildOffset),
               ValueRange{key}));
  auto loop = scf::ForOp::create(rewriter, loc, zero, number, one,
                                 ValueRange{first});
  rewriter.setInsertionPointToStart(loop.getBody());
  Value slot =
      arith::SubIOp::create(rewriter, loc, loop.getRegionIterArg(0), one);
  Value next = world.toIndex(
      loc, memref::LoadOp::create(
               rewriter, loc,
               world.treeLinks(relation, relation.nextSiblingOffset),
               ValueRange{slot}));
  auto yield = scf::YieldOp::create(rewriter, loc, ValueRange{next});
  rewriter.setInsertionPoint(yield);
  return {loop, slot};
}

/// Combine the values an apply inside `ent.edges` sent from the first
/// `count` rows of `archetype`, at the insertion point, row after row and
/// edge after edge: rows that ran the loop (their flag is 0) visited
/// exactly the edges of their entity's range, each of which holds a target
/// id ("no target" where the apply did not run) and a value.
static void combineEdgeApplied(IRRewriter &rewriter, ApplyOp apply,
                               const WorldLayout &layout,
                               const WorldArchetype &archetype,
                               WorldAccess &world, Value count, Value tick) {
  Location loc = apply.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  auto edges = apply->getParentOfType<EdgesOp>();
  const WorldRelation &relation =
      layout.getRelation(edges.getRelationAttr().getAttr());
  Value flags = world.applyBuffer(apply, archetype).first;
  auto [ids, values] = world.edgeApplyBuffer(apply);
  LocateBounds bounds = loadApplyBounds(rewriter, loc, apply, layout, world);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  auto rows = scf::ForOp::create(rewriter, loc, zero, count, one);
  rewriter.setInsertionPoint(rows.getBody()->getTerminator());
  Value row = rows.getInductionVar();
  Value flag = memref::LoadOp::create(rewriter, loc, flags, ValueRange{row});
  Value ran = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                    flag, world.noEntity(loc));
  auto ifRan = scf::IfOp::create(rewriter, loc, ran);
  rewriter.setInsertionPointToStart(ifRan.thenBlock());
  Value own = world.entityId(loc, archetype, row);
  Value key = world.entityKey(loc, own);
  Value position;
  if (relation.linked) {
    position = emitLinkedEdges(rewriter, loc, world, relation,
                               !edges.isOut(), key, own)
                   .second;
  } else {
    auto [begin, end] =
        emitEdgeRange(rewriter, loc, world, relation, !edges.isOut(), key);
    auto positions = scf::ForOp::create(rewriter, loc, begin, end, one);
    rewriter.setInsertionPoint(positions.getBody()->getTerminator());
    position = positions.getInductionVar();
  }
  Value id = memref::LoadOp::create(rewriter, loc, ids, ValueRange{position});
  Value sent = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                     id, world.noEntity(loc));
  auto ifSent = scf::IfOp::create(rewriter, loc, sent);
  rewriter.setInsertionPointToStart(ifSent.thenBlock());
  Value value =
      memref::LoadOp::create(rewriter, loc, values, ValueRange{position});
  combineInto(rewriter, apply, layout, world, id, value, tick, bounds);
}

/// Replace `edges` (cloned into the body of a query for `row` of
/// `archetype`) by a loop over the edges of the entity at that row, and
/// return the loop. Get and set through the edge ref become loads and
/// stores at the edge's position in the table, applies write the edge's
/// slot of their buffers, and disconnects mark the edge dead and the
/// relation unclean. With generational ids a slot may have been reused
/// since the edges were last sorted: only edges whose own end is this
/// entity's id count.
static Operation *lowerEdges(IRRewriter &rewriter, EdgesOp edges,
                             const WorldArchetype &archetype,
                             WorldAccess &world, const WorldLayout &layout,
                             Value row, Value tick, bool directApplies) {
  Location loc = edges.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(edges);
  const WorldRelation &relation =
      layout.getRelation(edges.getRelationAttr().getAttr());
  bool out = edges.isOut();
  Value id = world.entityId(loc, archetype, row);
  Value key = world.entityKey(loc, id);
  SmallVector<ApplyOp> applies, direct;
  edges.walk([&](ApplyOp apply) {
    (appliesDirectly(apply, directApplies) ? direct : applies)
        .push_back(apply);
  });
  // The row ran this loop: its edges' slots are to be combined.
  for (ApplyOp apply : applies)
    memref::StoreOp::create(
        rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0,
                                                    layout.entities.idBits),
        world.applyBuffer(apply, archetype).first, ValueRange{row});
  scf::ForOp loop;
  Value position, edge;
  if (relation.linked) {
    // Its slot, or the slots of the edges to it; a slot is also the
    // edge's place in an apply's buffers.
    std::tie(loop, edge) =
        emitLinkedEdges(rewriter, loc, world, relation, !out, key, id);
    position = edge;
  } else {
    auto [begin, end] =
        emitEdgeRange(rewriter, loc, world, relation, !out, key);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    loop = scf::ForOp::create(rewriter, loc, begin, end, one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    position = loop.getInductionVar();
    edge = relation.isSorted(!out)
               ? position
               : world.toIndex(loc, memref::LoadOp::create(
                                        rewriter, loc,
                                        world.indexEdges(relation),
                                        ValueRange{position}));
  }
  for (ApplyOp apply : applies)
    memref::StoreOp::create(rewriter, loc, world.noEntity(loc),
                            world.edgeApplyBuffer(apply).first,
                            ValueRange{position});
  Operation *insertBefore = loop.getBody()->getTerminator();
  // A visited entity whose end is trusted never dies, so its slot is never
  // reused and its range holds only its own edges.
  bool ownEndTrusted =
      relation.getTrusted(/*target=*/!out) != FlatSymbolRefAttr();
  // (A linked tree's edge out is its entity's or not there.)
  if (layout.entities.hasGenerations() && !ownEndTrusted &&
      !(relation.linked && out)) {
    Value own = memref::LoadOp::create(
        rewriter, loc, world.edgeIds(relation, /*source=*/out),
        ValueRange{edge});
    Value mine = arith::CmpIOp::create(rewriter, loc,
                                       arith::CmpIPredicate::eq, own, id);
    auto ifMine = scf::IfOp::create(rewriter, loc, mine);
    insertBefore = ifMine.thenBlock()->getTerminator();
  }
  rewriter.setInsertionPoint(insertBefore);
  Value otherId = memref::LoadOp::create(
      rewriter, loc, world.edgeIds(relation, /*source=*/!out),
      ValueRange{edge});
  // A linked tree's slot has its source as its owner, the id and one.
  if (relation.linked && !out)
    otherId = world.ownerId(loc, otherId);
  Value other = world.fromStorage(loc, otherId,
                                  EntityType::get(rewriter.getContext()));

  Block &body = edges.getBody().front();
  for (Operation *user :
       llvm::make_early_inc_range(body.getArgument(0).getUsers())) {
    rewriter.setInsertionPoint(user);
    if (auto get = dyn_cast<GetOp>(user)) {
      const WorldColumn *field = relation.find(get.getFieldAttr());
      Value value = memref::LoadOp::create(
          rewriter, get.getLoc(), world.edgeField(relation, *field),
          ValueRange{edge});
      rewriter.replaceOp(get,
                         world.fromStorage(get.getLoc(), value, get.getType()));
    } else {
      auto set = cast<SetOp>(user);
      const WorldColumn *field = relation.find(set.getFieldAttr());
      memref::StoreOp::create(rewriter, set.getLoc(),
                              world.toStorage(set.getLoc(), set.getValue()),
                              world.edgeField(relation, *field),
                              ValueRange{edge});
      rewriter.eraseOp(set);
    }
  }
  for (ApplyOp apply : direct) {
    rewriter.setInsertionPoint(apply);
    combineDirectly(rewriter, apply, layout, world,
                    world.toStorage(apply.getLoc(), apply.getEntity()),
                    world.toStorage(apply.getLoc(), apply.getValue()), tick);
    rewriter.eraseOp(apply);
  }
  for (ApplyOp apply : applies) {
    rewriter.setInsertionPoint(apply);
    auto [ids, values] = world.edgeApplyBuffer(apply);
    memref::StoreOp::create(rewriter, apply.getLoc(),
                            world.toStorage(apply.getLoc(), apply.getEntity()),
                            ids, ValueRange{position});
    memref::StoreOp::create(rewriter, apply.getLoc(),
                            world.toStorage(apply.getLoc(), apply.getValue()),
                            values, ValueRange{position});
    rewriter.eraseOp(apply);
  }
  SmallVector<DisconnectOp> disconnects;
  body.walk([&](DisconnectOp op) { disconnects.push_back(op); });
  for (DisconnectOp disconnect : disconnects) {
    Location at = disconnect.getLoc();
    rewriter.setInsertionPoint(disconnect);
    memref::StoreOp::create(rewriter, at,
                            arith::ConstantIntOp::create(rewriter, at, 1, 8),
                            world.edgeDead(relation), ValueRange{edge});
    // (The edge's target loses a child.)
    if (relation.childTicksOffset) {
      memref::StoreOp::create(
          rewriter, at, world.currentTick(at), world.childTicks(relation),
          ValueRange{out ? world.entityKey(at, otherId) : key});
      // (Bodies may run on several threads, which the ring of those
      // that changed is not made for: it says it has lost this one.)
      if (relation.touchedOffset)
        memref::StoreOp::create(
            rewriter, at, world.currentTick(at), world.touchedState(relation),
            ValueRange{arith::ConstantIndexOp::create(rewriter, at, 1)});
    }
    Value zero = arith::ConstantIndexOp::create(rewriter, at, 0);
    memref::StoreOp::create(rewriter, at,
                            arith::ConstantIntOp::create(rewriter, at, 0, 64),
                            world.edgesClean(relation), ValueRange{zero});
    rewriter.eraseOp(disconnect);
  }
  rewriter.eraseOp(body.getTerminator());
  body.eraseArgument(0);
  rewriter.inlineBlockBefore(&body, insertBefore, ValueRange{other});
  rewriter.eraseOp(edges);
  return loop;
}

/// A field of the visited entity that an edge loop sets: carried through
/// the loop as a value.
namespace {
struct CarriedField {
  Value ref;
  StringAttr field;
  Type type;
};
/// The carried fields' values and whether they were set, at a point of the
/// loop body.
struct CarriedState {
  SmallVector<Value> values;
  SmallVector<Value> written;
};
} // namespace

static int findCarried(ArrayRef<CarriedField> fields, Value ref,
                       StringAttr field) {
  for (auto [index, carried] : llvm::enumerate(fields))
    if (carried.ref == ref && carried.field == field)
      return index;
  return -1;
}

/// Replace the gets and sets of `fields` in `block` by the values `state`
/// carries, threading them through the `scf.if`s that contain any.
static void carryThrough(IRRewriter &rewriter, Block &block,
                         ArrayRef<CarriedField> fields, CarriedState &state) {
  auto touches = [&](Operation *op) {
    return op
        ->walk([&](Operation *nested) {
          if (auto get = dyn_cast<GetOp>(nested);
              get && findCarried(fields, get.getRef(), get.getFieldAttr()) >= 0)
            return WalkResult::interrupt();
          if (auto set = dyn_cast<SetOp>(nested);
              set && findCarried(fields, set.getRef(), set.getFieldAttr()) >= 0)
            return WalkResult::interrupt();
          return WalkResult::advance();
        })
        .wasInterrupted();
  };
  for (Operation &op : llvm::make_early_inc_range(block)) {
    if (auto get = dyn_cast<GetOp>(op)) {
      int index = findCarried(fields, get.getRef(), get.getFieldAttr());
      if (index >= 0) {
        rewriter.replaceOp(get, state.values[index]);
        continue;
      }
    }
    if (auto set = dyn_cast<SetOp>(op)) {
      int index = findCarried(fields, set.getRef(), set.getFieldAttr());
      if (index >= 0) {
        rewriter.setInsertionPoint(set);
        state.values[index] = set.getValue();
        state.written[index] =
            arith::ConstantIntOp::create(rewriter, set.getLoc(), 1, 1);
        rewriter.eraseOp(set);
        continue;
      }
    }
    auto branch = dyn_cast<scf::IfOp>(op);
    if (!branch || !touches(branch))
      continue;
    CarriedState thenState = state, elseState = state;
    carryThrough(rewriter, *branch.thenBlock(), fields, thenState);
    if (branch.elseBlock())
      carryThrough(rewriter, *branch.elseBlock(), fields, elseState);
    // The fields either branch changes become results of the `if`.
    SmallVector<unsigned> changed;
    for (unsigned i = 0; i < fields.size(); ++i)
      if (thenState.values[i] != state.values[i] ||
          elseState.values[i] != state.values[i] ||
          thenState.written[i] != state.written[i] ||
          elseState.written[i] != state.written[i])
        changed.push_back(i);
    if (changed.empty())
      continue;
    Location loc = branch.getLoc();
    SmallVector<Type> types(branch.getResultTypes());
    for (unsigned i : changed) {
      types.push_back(fields[i].type);
      types.push_back(rewriter.getI1Type());
    }
    rewriter.setInsertionPoint(branch);
    auto replacement = scf::IfOp::create(rewriter, loc, types,
                                         branch.getCondition(),
                                         /*withElseRegion=*/true);
    // Blocks of a new `if` with results come without terminators.
    for (Block *block : {replacement.thenBlock(), replacement.elseBlock()})
      while (!block->empty())
        rewriter.eraseOp(&block->back());
    rewriter.eraseBlock(replacement.thenBlock());
    replacement.getThenRegion().takeBody(branch.getThenRegion());
    if (branch.elseBlock()) {
      rewriter.eraseBlock(replacement.elseBlock());
      replacement.getElseRegion().takeBody(branch.getElseRegion());
    } else {
      rewriter.setInsertionPointToEnd(replacement.elseBlock());
      scf::YieldOp::create(rewriter, loc);
    }
    for (auto [block, branchState] :
         {std::make_pair(replacement.thenBlock(), &thenState),
          std::make_pair(replacement.elseBlock(), &elseState)}) {
      auto yield = cast<scf::YieldOp>(block->getTerminator());
      SmallVector<Value> operands(yield.getOperands());
      for (unsigned i : changed) {
        operands.push_back(branchState->values[i]);
        operands.push_back(branchState->written[i]);
      }
      rewriter.setInsertionPoint(yield);
      rewriter.replaceOpWithNewOp<scf::YieldOp>(yield, operands);
    }
    unsigned results = branch.getNumResults();
    rewriter.replaceOp(branch,
                       replacement.getResults().take_front(results));
    for (auto [k, i] : llvm::enumerate(changed)) {
      state.values[i] = replacement.getResult(results + 2 * k);
      state.written[i] = replacement.getResult(results + 2 * k + 1);
    }
  }
}

/// Keep the fields of the visited entity that the edge loop `loop` sets in
/// registers: load each once before the loop, carry it through the loop
/// (and the `scf.if`s in it) as a value, and set it once after the loop if
/// any iteration set it. LLVM cannot do this itself: every column is a view
/// of the same arena, so a store to the entity's field might change any
/// other load in the loop. The language rules it out: in an edge loop only
/// the entity's own get and set reach its fields (a query may not look up a
/// field it writes, applies land when the query ends). Fields set inside
/// other regions than `scf.if`, or of a component the loop adds, stay in
/// memory. Returns the ops that replace `loop`, in order.
static SmallVector<Operation *> carryOwnFields(IRRewriter &rewriter,
                                               scf::ForOp loop) {
  SmallVector<CarriedField> fields;
  loop.walk([&](SetOp set) {
    auto arg = dyn_cast<BlockArgument>(set.getRef());
    if (!arg || !isa<QueryOp>(arg.getOwner()->getParentOp()) ||
        findCarried(fields, set.getRef(), set.getFieldAttr()) >= 0)
      return;
    fields.push_back(
        {set.getRef(), set.getFieldAttr(), set.getValue().getType()});
  });
  // Every get and set of a carried field must sit in `scf.if`s only.
  auto onlyIfs = [&](Operation *op) {
    for (Operation *parent = op->getParentOp(); parent != loop;
         parent = parent->getParentOp())
      if (!isa<scf::IfOp>(parent))
        return false;
    return true;
  };
  llvm::SmallPtrSet<Attribute, 4> added;
  loop.walk([&](AddOp add) { added.insert(add.getComponentAttr()); });
  loop.walk([&](Operation *op) {
    Value ref;
    StringAttr field;
    if (auto get = dyn_cast<GetOp>(op))
      ref = get.getRef(), field = get.getFieldAttr();
    else if (auto set = dyn_cast<SetOp>(op))
      ref = set.getRef(), field = set.getFieldAttr();
    else
      return;
    int index = findCarried(fields, ref, field);
    if (index >= 0 && !onlyIfs(op))
      fields.erase(fields.begin() + index);
  });
  llvm::erase_if(fields, [&](const CarriedField &carried) {
    return added.contains(
        cast<RefType>(carried.ref.getType()).getComponent());
  });
  if (fields.empty())
    return {loop};

  Location loc = loop.getLoc();
  SmallVector<Operation *> ops;
  rewriter.setInsertionPoint(loop);
  SmallVector<Value> inits;
  for (const CarriedField &carried : fields) {
    auto get = GetOp::create(rewriter, loc, carried.type, carried.ref,
                             carried.field);
    ops.push_back(get);
    inits.push_back(get);
  }
  Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
  for (unsigned i = 0; i < fields.size(); ++i)
    inits.push_back(no);
  auto carrying = scf::ForOp::create(rewriter, loc, loop.getLowerBound(),
                                     loop.getUpperBound(), loop.getStep(),
                                     inits);
  ops.push_back(carrying);
  Block *body = carrying.getBody();
  while (!body->empty())
    rewriter.eraseOp(&body->back());
  body->getOperations().splice(body->end(),
                               loop.getBody()->getOperations());
  rewriter.replaceAllUsesWith(loop.getInductionVar(),
                              carrying.getInductionVar());
  CarriedState state;
  for (unsigned i = 0; i < fields.size(); ++i) {
    state.values.push_back(carrying.getRegionIterArg(i));
    state.written.push_back(carrying.getRegionIterArg(fields.size() + i));
  }
  carryThrough(rewriter, *body, fields, state);
  auto yield = cast<scf::YieldOp>(body->getTerminator());
  SmallVector<Value> operands(state.values);
  operands.append(state.written.begin(), state.written.end());
  rewriter.setInsertionPoint(yield);
  rewriter.replaceOpWithNewOp<scf::YieldOp>(yield, operands);
  rewriter.eraseOp(loop);

  rewriter.setInsertionPointAfter(carrying);
  for (auto [i, carried] : llvm::enumerate(fields)) {
    auto ifWritten = scf::IfOp::create(
        rewriter, loc, carrying.getResult(fields.size() + i));
    ops.push_back(ifWritten);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(ifWritten.thenBlock());
    SetOp::create(rewriter, loc, carried.ref, carried.field,
                  carrying.getResult(i));
  }
  return ops;
}

/// Where both branches of `branch` store to the same column at the same
/// index, yield the two values and store once after the `if`: the stores no
/// longer depend on the branch, so it can become a select and the loop
/// around it be vectorised (LLVM does not merge them itself: each store
/// computes its address in its own block). Only for an `if` whose branches
/// do nothing but compute and store, and where no other store in either
/// branch writes a column a merged store writes (columns are disjoint
/// views, so stores to different ones never alias).
static void mergeBranchStores(IRRewriter &rewriter, scf::IfOp branch) {
  if (!branch.elseBlock())
    return;
  auto storesOnly = [](Block *block) {
    for (Operation &op : block->without_terminator())
      if (!isa<memref::StoreOp>(op) &&
          !(op.getNumRegions() == 0 && isMemoryEffectFree(&op)))
        return false;
    return true;
  };
  if (!storesOnly(branch.thenBlock()) || !storesOnly(branch.elseBlock()))
    return;
  SmallVector<std::pair<memref::StoreOp, memref::StoreOp>> pairs;
  for (auto store : branch.thenBlock()->getOps<memref::StoreOp>())
    for (auto other : branch.elseBlock()->getOps<memref::StoreOp>())
      if (store.getMemref() == other.getMemref() &&
          store.getIndices() == other.getIndices() &&
          llvm::none_of(pairs, [&](auto &pair) {
            return pair.first.getMemref() == store.getMemref();
          }))
        pairs.push_back({store, other});
  // Every other store must leave the merged columns alone, and a merged
  // column is stored once per branch.
  for (Block *block : {branch.thenBlock(), branch.elseBlock()})
    for (auto store : block->getOps<memref::StoreOp>()) {
      auto *pair = llvm::find_if(pairs, [&](auto &pair) {
        return pair.first.getMemref() == store.getMemref();
      });
      if (pair != pairs.end() && pair->first != store &&
          pair->second != store)
        return;
    }
  // The index of a merged store must be defined outside the `if`.
  llvm::erase_if(pairs, [&](auto &pair) {
    return llvm::any_of(pair.first.getIndices(), [&](Value index) {
      return branch->isProperAncestor(index.getParentBlock()->getParentOp()) ||
             index.getParentBlock()->getParentOp() == branch;
    });
  });
  if (pairs.empty())
    return;

  Location loc = branch.getLoc();
  SmallVector<Type> types(branch.getResultTypes());
  for (auto &pair : pairs)
    types.push_back(pair.first.getValueToStore().getType());
  rewriter.setInsertionPoint(branch);
  auto replacement = scf::IfOp::create(rewriter, loc, types,
                                       branch.getCondition(),
                                       /*withElseRegion=*/true);
  for (Block *block : {replacement.thenBlock(), replacement.elseBlock()})
    while (!block->empty())
      rewriter.eraseOp(&block->back());
  rewriter.eraseBlock(replacement.thenBlock());
  replacement.getThenRegion().takeBody(branch.getThenRegion());
  rewriter.eraseBlock(replacement.elseBlock());
  replacement.getElseRegion().takeBody(branch.getElseRegion());
  for (bool then : {true, false}) {
    Block *block = then ? replacement.thenBlock() : replacement.elseBlock();
    auto yield = cast<scf::YieldOp>(block->getTerminator());
    SmallVector<Value> operands(yield.getOperands());
    for (auto &pair : pairs) {
      memref::StoreOp store = then ? pair.first : pair.second;
      operands.push_back(store.getValueToStore());
    }
    rewriter.setInsertionPoint(yield);
    rewriter.replaceOpWithNewOp<scf::YieldOp>(yield, operands);
  }
  unsigned results = branch.getNumResults();
  rewriter.setInsertionPointAfter(replacement);
  for (auto [k, pair] : llvm::enumerate(pairs)) {
    memref::StoreOp::create(rewriter, loc, replacement.getResult(results + k),
                            pair.first.getMemref(), pair.first.getIndices());
    rewriter.eraseOp(pair.first);
    rewriter.eraseOp(pair.second);
  }
  rewriter.replaceOp(branch, replacement.getResults().take_front(results));
}

/// Whether `value` is the constant index 0.
static bool isZeroIndex(Value value) {
  auto constant = value.getDefiningOp<arith::ConstantIndexOp>();
  return constant && constant.value() == 0;
}

/// Replace the loads and stores of the one-element `cells` in `block` by the
/// values `state` carries, threading them through the `scf.if`s that
/// contain any (as carryThrough does for fields).
static void carryCellsThrough(IRRewriter &rewriter, Block &block,
                              ArrayRef<Value> cells, CarriedState &state) {
  auto cellOf = [&](Operation *op) -> int {
    Value memref;
    if (auto load = dyn_cast<memref::LoadOp>(op))
      memref = load.getMemref();
    else if (auto store = dyn_cast<memref::StoreOp>(op))
      memref = store.getMemref();
    else
      return -1;
    auto *it = llvm::find(cells, memref);
    return it == cells.end() ? -1 : int(it - cells.begin());
  };
  auto touches = [&](Operation *op) {
    return op
        ->walk([&](Operation *nested) {
          return cellOf(nested) >= 0 ? WalkResult::interrupt()
                                     : WalkResult::advance();
        })
        .wasInterrupted();
  };
  for (Operation &op : llvm::make_early_inc_range(block)) {
    int index = cellOf(&op);
    if (auto load = dyn_cast<memref::LoadOp>(op); load && index >= 0) {
      rewriter.replaceOp(load, state.values[index]);
      continue;
    }
    if (auto store = dyn_cast<memref::StoreOp>(op); store && index >= 0) {
      rewriter.setInsertionPoint(store);
      state.values[index] = store.getValueToStore();
      state.written[index] =
          arith::ConstantIntOp::create(rewriter, store.getLoc(), 1, 1);
      rewriter.eraseOp(store);
      continue;
    }
    auto branch = dyn_cast<scf::IfOp>(op);
    if (!branch || !touches(branch))
      continue;
    CarriedState thenState = state, elseState = state;
    carryCellsThrough(rewriter, *branch.thenBlock(), cells, thenState);
    if (branch.elseBlock())
      carryCellsThrough(rewriter, *branch.elseBlock(), cells, elseState);
    SmallVector<unsigned> changed;
    for (unsigned i = 0; i < cells.size(); ++i)
      if (thenState.values[i] != state.values[i] ||
          elseState.values[i] != state.values[i] ||
          thenState.written[i] != state.written[i] ||
          elseState.written[i] != state.written[i])
        changed.push_back(i);
    if (changed.empty())
      continue;
    Location loc = branch.getLoc();
    SmallVector<Type> types(branch.getResultTypes());
    for (unsigned i : changed) {
      types.push_back(state.values[i].getType());
      types.push_back(rewriter.getI1Type());
    }
    rewriter.setInsertionPoint(branch);
    auto replacement = scf::IfOp::create(rewriter, loc, types,
                                         branch.getCondition(),
                                         /*withElseRegion=*/true);
    for (Block *block : {replacement.thenBlock(), replacement.elseBlock()})
      while (!block->empty())
        rewriter.eraseOp(&block->back());
    rewriter.eraseBlock(replacement.thenBlock());
    replacement.getThenRegion().takeBody(branch.getThenRegion());
    if (branch.elseBlock()) {
      rewriter.eraseBlock(replacement.elseBlock());
      replacement.getElseRegion().takeBody(branch.getElseRegion());
    } else {
      rewriter.setInsertionPointToEnd(replacement.elseBlock());
      scf::YieldOp::create(rewriter, loc);
    }
    for (auto [block, branchState] :
         {std::make_pair(replacement.thenBlock(), &thenState),
          std::make_pair(replacement.elseBlock(), &elseState)}) {
      auto yield = cast<scf::YieldOp>(block->getTerminator());
      SmallVector<Value> operands(yield.getOperands());
      for (unsigned i : changed) {
        operands.push_back(branchState->values[i]);
        operands.push_back(branchState->written[i]);
      }
      rewriter.setInsertionPoint(yield);
      rewriter.replaceOpWithNewOp<scf::YieldOp>(yield, operands);
    }
    unsigned results = branch.getNumResults();
    rewriter.replaceOp(branch, replacement.getResults().take_front(results));
    for (auto [k, i] : llvm::enumerate(changed)) {
      state.values[i] = replacement.getResult(results + 2 * k);
      state.written[i] = replacement.getResult(results + 2 * k + 1);
    }
  }
}

/// Keep the resource cells that directly combined accumulates write in
/// registers across the entity loop `loop`: load each once before it, carry
/// it through the loop as a value and store it once after (unchanged if no
/// entity combined into it; resources carry no change stamps). No flag says
/// whether one did: LLVM would specialise the loop on it, giving it early
/// exits, and not vectorise it. Only the accumulate reaches the cell inside the loop
/// (nothing else in its query reads the field, see kUnobservedAttr), but
/// LLVM cannot tell its store from the columns', all views of the arena,
/// so it kept the loop scalar and branching. A cell accessed inside other
/// regions than `scf.if`, or at another index than 0, stays in memory.
static scf::ForOp carryCells(IRRewriter &rewriter, scf::ForOp loop,
                             SmallVector<Value> cells) {
  llvm::erase_if(cells, [&](Value cell) {
    WalkResult result = loop.getBody()->walk([&](Operation *op) {
      Value memref;
      ValueRange indices;
      if (auto load = dyn_cast<memref::LoadOp>(op))
        memref = load.getMemref(), indices = load.getIndices();
      else if (auto store = dyn_cast<memref::StoreOp>(op))
        memref = store.getMemref(), indices = store.getIndices();
      if (memref != cell)
        return WalkResult::advance();
      for (Operation *parent = op->getParentOp(); parent != loop;
           parent = parent->getParentOp())
        if (!isa<scf::IfOp>(parent))
          return WalkResult::interrupt();
      if (indices.size() != 1 || !isZeroIndex(indices[0]))
        return WalkResult::interrupt();
      return WalkResult::advance();
    });
    bool used = false;
    loop.getBody()->walk([&](Operation *op) {
      used |= llvm::is_contained(op->getOperands(), cell);
    });
    return result.wasInterrupted() || !used;
  });
  if (cells.empty())
    return loop;

  Location loc = loop.getLoc();
  rewriter.setInsertionPoint(loop);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  SmallVector<Value> inits;
  for (Value cell : cells)
    inits.push_back(
        memref::LoadOp::create(rewriter, loc, cell, ValueRange{zero}));
  auto carrying = scf::ForOp::create(rewriter, loc, loop.getLowerBound(),
                                     loop.getUpperBound(), loop.getStep(),
                                     inits);
  Block *body = carrying.getBody();
  while (!body->empty())
    rewriter.eraseOp(&body->back());
  body->getOperations().splice(body->end(), loop.getBody()->getOperations());
  rewriter.replaceAllUsesWith(loop.getInductionVar(),
                              carrying.getInductionVar());
  // carryCellsThrough tracks a written flag too; here it is not used.
  CarriedState state;
  Value unused = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
  rewriter.moveOpBefore(unused.getDefiningOp(), carrying);
  for (unsigned i = 0; i < cells.size(); ++i) {
    state.values.push_back(carrying.getRegionIterArg(i));
    state.written.push_back(unused);
  }
  carryCellsThrough(rewriter, *body, cells, state);
  auto yield = cast<scf::YieldOp>(body->getTerminator());
  rewriter.setInsertionPoint(yield);
  rewriter.replaceOpWithNewOp<scf::YieldOp>(yield, state.values);
  rewriter.replaceOp(loop, ValueRange{});
  rewriter.setInsertionPointAfter(carrying);
  for (auto [i, cell] : llvm::enumerate(cells))
    memref::StoreOp::create(rewriter, loc, carrying.getResult(i), cell,
                            ValueRange{zero});
  return carrying;
}

/// Append the edge (`source`, `target`, `values`, all stored forms) to
/// `relation` at the insertion point, after checking its capacity, and
/// mark it unclean.
/// The name of the function that connects an edge of `relation`, a
/// linked tree: (source, target, the fields' values, the arena).
static std::string connectFunctionName(const WorldRelation &relation) {
  return ("ent_connect_" + RelationOp(relation.op).getSymName()).str();
}

static void appendEdge(IRRewriter &rewriter, Location loc,
                       const WorldLayout &layout, WorldAccess &world,
                       const WorldRelation &relation, Value source,
                       Value target, ValueRange values);

/// The ends of an edge must have the components the relation names.
static void assertEnds(IRRewriter &rewriter, Location loc,
                       const WorldLayout &layout, WorldAccess &world,
                       const WorldRelation &relation, Value source,
                       Value target) {
  RelationOp relationOp = relation.op;
  for (auto [end, id] : {std::make_pair(false, source),
                         std::make_pair(true, target)}) {
    FlatSymbolRefAttr component = relationOp.getEndpoint(end);
    if (!component)
      continue;
    auto holds = [&](const WorldArchetype &archetype) {
      return ArchetypeOp(archetype.op).contains(component);
    };
    Value has = emitLocate(
        rewriter, loc, layout, world, id, holds, component,
        TypeRange{rewriter.getI1Type()},
        [&](const WorldArchetype &, Value, Value present) -> SmallVector<Value> {
          if (!present)
            present = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
          return {present};
        },
        [&]() -> SmallVector<Value> {
          return {arith::ConstantIntOp::create(rewriter, loc, 0, 1)};
        })[0];
    cf::AssertOp::create(
        rewriter, loc, has,
        rewriter.getStringAttr(
            "ent.connect: the " + std::string(end ? "target" : "source") +
            " of an edge of @" + relationOp.getSymName().str() +
            " does not have @" + component.getValue().str()));
  }
}

static void appendEdge(IRRewriter &rewriter, Location loc,
                       const WorldLayout &layout, WorldAccess &world,
                       const WorldRelation &relation, Value source,
                       Value target, ValueRange values) {
  assertEnds(rewriter, loc, layout, world, relation, source, target);
  // Being connected is an event of the source, where a reactive query has
  // a trigger up this tree: what it sees through a ref up it is another
  // entity's from now on.
  if (relation.connectedOffset) {
    Value key = world.entityKey(loc, source);
    Value ticks = world.connectedTicks(relation);
    Value now = world.currentTick(loc);
    Value old = memref::LoadOp::create(rewriter, loc, ticks, ValueRange{key});
    memref::StoreOp::create(rewriter, loc, now, ticks, ValueRange{key});
    memref::StoreOp::create(rewriter, loc, now, ticks,
                            ValueRange{world.latestConnect(loc)});
    Stamp stamp{Trigger::Connected, RelationOp(relation.op).getSymNameAttr(),
                rewriter.getStringAttr("")};
    if (const WorldLog *log = layout.findLog(stamp))
      appendToLog(
          rewriter, loc, world, *log,
          arith::AndIOp::create(
              rewriter, loc,
              arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                         key),
              arith::ConstantIntOp::create(rewriter, loc, log->segments - 1,
                                           64)),
          source, now, old, Value(), /*atomic=*/false);
  }
  // A linked tree takes the edge in at once.
  if (relation.linked) {
    SmallVector<Value> arguments{source, target};
    arguments.append(values.begin(), values.end());
    arguments.push_back(world.getArena());
    func::CallOp::create(rewriter, loc, connectFunctionName(relation),
                         TypeRange{}, arguments);
    return;
  }
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  OpBuilder::InsertionGuard guard(rewriter);
  // Among children with the same order, the one connected later is later.
  if (relation.isOrdered()) {
    Value numbers = world.connectNumbers(relation);
    Value count =
        arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys);
    Value number = arith::AddIOp::create(
        rewriter, loc,
        memref::LoadOp::create(rewriter, loc, numbers, ValueRange{count}),
        arith::ConstantIntOp::create(rewriter, loc, 1, 64));
    memref::StoreOp::create(rewriter, loc, number, numbers, ValueRange{count});
    memref::StoreOp::create(rewriter, loc, number, numbers,
                            ValueRange{world.entityKey(loc, source)});
  }
  // A sorted tree: an entity that had an edge when the table was last
  // sorted has it at its offset, and gets the new one there. So changing
  // a parent takes no room in the table, which a tree with as many edges
  // as it may have has none of. (The table and what is made from it are
  // put in order again by the sort, as for any connect.)
  if (relation.tree) {
    auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                      /*in=*/false,
                                      world.entityKey(loc, source));
    Value sorted = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, world.sortedCount(relation),
                                    ValueRange{zero}));
    Value inTable = arith::AndIOp::create(
        rewriter, loc,
        arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult, begin,
                              end),
        arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult, begin,
                              sorted));
    auto ifInTable = scf::IfOp::create(rewriter, loc,
                                       TypeRange{rewriter.getI1Type()},
                                       inTable, /*withElseRegion=*/true);
    rewriter.setInsertionPointToStart(ifInTable.thenBlock());
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq,
            memref::LoadOp::create(rewriter, loc,
                                   world.edgeIds(relation, /*source=*/true),
                                   ValueRange{begin}),
            source)});
    rewriter.setInsertionPointToStart(ifInTable.elseBlock());
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::ConstantIntOp::create(rewriter, loc, 0, 1)});
    rewriter.setInsertionPointAfter(ifInTable);
    auto ifHas = scf::IfOp::create(rewriter, loc, ifInTable.getResult(0),
                                   /*withElseRegion=*/true);
    rewriter.setInsertionPointToStart(ifHas.thenBlock());
    memref::StoreOp::create(rewriter, loc, target,
                            world.edgeIds(relation, /*source=*/false),
                            ValueRange{begin});
    for (auto [value, field] : llvm::zip(values, relation.fields))
      memref::StoreOp::create(rewriter, loc, value,
                              world.edgeField(relation, field),
                              ValueRange{begin});
    if (relation.deadOffset)
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 8),
          world.edgeDead(relation), ValueRange{begin});
    memref::StoreOp::create(rewriter, loc,
                            arith::ConstantIntOp::create(rewriter, loc, 0, 64),
                            world.edgesClean(relation), ValueRange{zero});
    rewriter.setInsertionPointToStart(ifHas.elseBlock());
  }
  Value counter = world.edgeCount(relation);
  Value count = memref::LoadOp::create(rewriter, loc, counter,
                                       ValueRange{zero});
  Value at = world.toIndex(loc, count);
  Value fits = arith::CmpIOp::create(
      rewriter, loc, arith::CmpIPredicate::ult, at,
      arith::ConstantIndexOp::create(rewriter, loc, relation.capacity));
  cf::AssertOp::create(
      rewriter, loc, fits,
      rewriter.getStringAttr("ent.connect exceeds the capacity of @" +
                             RelationOp(relation.op).getSymName()));
  memref::StoreOp::create(rewriter, loc, source,
                          world.edgeIds(relation, /*source=*/true),
                          ValueRange{at});
  memref::StoreOp::create(rewriter, loc, target,
                          world.edgeIds(relation, /*source=*/false),
                          ValueRange{at});
  for (auto [value, field] : llvm::zip(values, relation.fields))
    memref::StoreOp::create(rewriter, loc, value,
                            world.edgeField(relation, field), ValueRange{at});
  if (relation.deadOffset)
    memref::StoreOp::create(rewriter, loc,
                            arith::ConstantIntOp::create(rewriter, loc, 0, 8),
                            world.edgeDead(relation), ValueRange{at});
  memref::StoreOp::create(
      rewriter, loc,
      arith::AddIOp::create(rewriter, loc, count,
                            arith::ConstantIntOp::create(rewriter, loc, 1, 64)),
      counter, ValueRange{zero});
  memref::StoreOp::create(rewriter, loc,
                          arith::ConstantIntOp::create(rewriter, loc, 0, 64),
                          world.edgesClean(relation), ValueRange{zero});
}

/// Append the edges `connect` (inside a query) buffered for the first
/// `count` rows of `archetype`, row after row, at the insertion point.
static void appendConnected(IRRewriter &rewriter, ConnectOp connect,
                            const WorldLayout &layout,
                            const WorldArchetype &archetype,
                            WorldAccess &world, Value count) {
  Location loc = connect.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  const WorldRelation &relation =
      layout.getRelation(connect.getRelationAttr().getAttr());
  WorldAccess::ConnectBuffers buffers =
      world.connectBuffer(connect, archetype);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
  rewriter.setInsertionPoint(loop.getBody()->getTerminator());
  Value row = loop.getInductionVar();
  Value source =
      memref::LoadOp::create(rewriter, loc, buffers.sources, ValueRange{row});
  Value added = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                      source, world.noEntity(loc));
  auto ifAdded = scf::IfOp::create(rewriter, loc, added);
  rewriter.setInsertionPointToStart(ifAdded.thenBlock());
  Value target =
      memref::LoadOp::create(rewriter, loc, buffers.targets, ValueRange{row});
  SmallVector<Value> values;
  for (Value column : buffers.values)
    values.push_back(
        memref::LoadOp::create(rewriter, loc, column, ValueRange{row}));
  appendEdge(rewriter, loc, layout, world, relation, source, target, values);
}

/// The name of the function that sorts `relation`'s edges if it is
/// unclean.
static std::string sortFunctionName(const WorldRelation &relation) {
  return ("ent_sort_" + RelationOp(relation.op).getSymName()).str();
}

static void callSort(IRRewriter &rewriter, Location loc,
                     const WorldRelation &relation, Value arena) {
  func::CallOp::create(rewriter, loc, sortFunctionName(relation), TypeRange{},
                       ValueRange{arena});
}

namespace {
/// What the functions of a linked tree are written with: loads and stores
/// in its per-key columns, and links, which are a key and one.
struct LinkedTree {
  IRRewriter &rewriter;
  Location loc;
  WorldAccess &world;
  const WorldRelation &relation;
  Value zero, one;

  LinkedTree(IRRewriter &rewriter, Location loc, WorldAccess &world,
             const WorldRelation &relation)
      : rewriter(rewriter), loc(loc), world(world), relation(relation) {
    zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  }

  Value load(Value column, Value at) {
    return memref::LoadOp::create(rewriter, loc, column, ValueRange{at});
  }
  void store(Value value, Value column, Value at) {
    memref::StoreOp::create(rewriter, loc, value, column, ValueRange{at});
  }
  Value links(uint64_t offset) { return world.treeLinks(relation, offset); }
  Value firstChild() { return links(relation.firstChildOffset); }
  Value lastChild() { return links(relation.lastChildOffset); }
  Value childCount() { return links(relation.childCountOffset); }
  Value nextSibling() { return links(relation.nextSiblingOffset); }
  Value previousSibling() { return links(relation.previousSiblingOffset); }
  Value position() { return links(relation.positionOffset); }
  Value sources() { return world.edgeIds(relation, /*source=*/true); }
  Value targets() { return world.edgeIds(relation, /*source=*/false); }
  /// A link, or a count, at the links' width, from an index and back.
  Value link(Value index) {
    return arith::IndexCastOp::create(rewriter, loc,
                                      world.offsetType(relation), index);
  }
  Value noLink() {
    return arith::ConstantIntOp::create(rewriter, loc, 0,
                                        relation.offsetBits);
  }
  Value isNone(Value linkValue) {
    return arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                 linkValue, noLink());
  }
  Value noOwner() {
    return arith::ConstantIntOp::create(rewriter, loc, 0,
                                        cast<IntegerType>(world.idType())
                                            .getWidth());
  }
  /// The id of the source of the edge in `slot`, which holds one.
  Value sourceOf(Value slot) {
    return world.ownerId(loc, load(sources(), slot));
  }
  Value i64(int64_t value) {
    return arith::ConstantIntOp::create(rewriter, loc, value, 64);
  }
  Value i1(bool value) {
    return arith::ConstantIntOp::create(rewriter, loc, value, 1);
  }
  Value negate(Value condition) {
    return arith::XOrIOp::create(rewriter, loc, condition, i1(true));
  }
  Value both(Value a, Value b) {
    return arith::AndIOp::create(rewriter, loc, a, b);
  }
  Value same(Value a, Value b) {
    return arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq, a,
                                 b);
  }

  /// `then` where `condition` holds, else `otherwise` if given.
  void branch(Value condition, function_ref<void()> then,
              function_ref<void()> otherwise = nullptr) {
    auto ifOp = scf::IfOp::create(rewriter, loc, condition,
                                  /*withElseRegion=*/bool(otherwise));
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(ifOp.thenBlock());
    then();
    if (otherwise) {
      rewriter.setInsertionPointToStart(ifOp.elseBlock());
      otherwise();
    }
  }
  void forEach(Value from, Value to, function_ref<void(Value)> body) {
    auto loop = scf::ForOp::create(rewriter, loc, from, to, one);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    body(loop.getInductionVar());
  }

  /// The slot `slot` joins the end of the children of the key `parent`.
  /// Whether it is the first is not to be foreseen where a tree is built
  /// again, and is known only once the parent's links have come from
  /// memory: so no branch on it. The first child is stored whichever it
  /// is, and the last child's next link or, if there is none, the slot's
  /// own once more.
  void linkUnder(Value slot, Value parent) {
    Value last = load(lastChild(), parent);
    Value self = link(arith::AddIOp::create(rewriter, loc, slot, one));
    Value none = isNone(last);
    store(last, previousSibling(), slot);
    store(noLink(), nextSibling(), slot);
    store(arith::SelectOp::create(rewriter, loc, none, self,
                                  load(firstChild(), parent)),
          firstChild(), parent);
    store(arith::SelectOp::create(rewriter, loc, none, noLink(), self),
          nextSibling(),
          arith::SelectOp::create(
              rewriter, loc, none, slot,
              arith::SubIOp::create(rewriter, loc, world.toIndex(loc, last),
                                    one)));
    store(self, lastChild(), parent);
    store(arith::AddIOp::create(
              rewriter, loc, load(childCount(), parent),
              arith::ConstantIntOp::create(rewriter, loc, 1,
                                           relation.offsetBits)),
          childCount(), parent);
  }
  /// What the children of an entity are ordered by, for the entity `id`
  /// (in its stored form): the tree's order field of it as an i64, 0
  /// where it has no such component.
  Value orderKey(const WorldLayout &layout, Value id) {
    Type wide = rewriter.getI64Type();
    FlatSymbolRefAttr component = FlatSymbolRefAttr::get(relation.orderComponent);
    // (No entity has one: all are 0.)
    if (llvm::none_of(layout.archetypes, [&](const WorldArchetype &archetype) {
          return ArchetypeOp(archetype.op).contains(component);
        }))
      return i64(0);
    return emitLocate(
        rewriter, loc, layout, world, id,
        [&](const WorldArchetype &archetype) {
          return ArchetypeOp(archetype.op).contains(component);
        },
        component, TypeRange{wide},
        [&](const WorldArchetype &archetype, Value row,
            Value present) -> SmallVector<Value> {
          Value value = memref::LoadOp::create(
              rewriter, loc,
              world.column(archetype, relation.orderComponent,
                           relation.orderField),
              ValueRange{row});
          if (value.getType() != wide)
            value = arith::ExtSIOp::create(rewriter, loc, wide, value);
          if (present)
            value = arith::SelectOp::create(rewriter, loc, present, value,
                                            i64(0));
          return {value};
        },
        [&]() -> SmallVector<Value> { return {i64(0)}; })[0];
  }
  /// As linkUnder, for a tree whose children are in an order: the slot
  /// goes after the last of them whose order is not greater than its own,
  /// found from the end (where it belongs if they come in order). What
  /// each is ordered by is in the relation's `orderKeys`.
  void linkInOrder(const WorldLayout &layout, Value slot, Value parent) {
    Value keys = world.orderKeys(relation);
    Value own = load(keys, slot);
    Type linkType = world.offsetType(relation);
    auto walk = scf::WhileOp::create(
        rewriter, loc, TypeRange{linkType},
        ValueRange{load(lastChild(), parent)},
        [&](OpBuilder &, Location, ValueRange state) {
          // On while there is a child here and it comes after the slot.
          auto ifAny = scf::IfOp::create(rewriter, loc,
                                         TypeRange{rewriter.getI1Type()},
                                         negate(isNone(state[0])),
                                         /*withElseRegion=*/true);
          {
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(ifAny.thenBlock());
            Value sibling = arith::SubIOp::create(
                rewriter, loc, world.toIndex(loc, state[0]), one);
            // (After it: a greater order, or the same and connected
            // later.)
            Value other = load(keys, sibling);
            Value numbers = world.connectNumbers(relation);
            scf::YieldOp::create(
                rewriter, loc,
                ValueRange{arith::OrIOp::create(
                    rewriter, loc,
                    arith::CmpIOp::create(rewriter, loc,
                                          arith::CmpIPredicate::sgt, other,
                                          own),
                    both(same(other, own),
                         arith::CmpIOp::create(
                             rewriter, loc, arith::CmpIPredicate::sgt,
                             load(numbers, sibling),
                             load(numbers, slot))))});
            rewriter.setInsertionPointToStart(ifAny.elseBlock());
            scf::YieldOp::create(rewriter, loc, ValueRange{i1(false)});
          }
          scf::ConditionOp::create(rewriter, loc, ifAny.getResult(0), state);
        },
        [&](OpBuilder &, Location, ValueRange state) {
          Value sibling = arith::SubIOp::create(
              rewriter, loc, world.toIndex(loc, state[0]), one);
          scf::YieldOp::create(rewriter, loc,
                               ValueRange{load(previousSibling(), sibling)});
        });
    Value after = walk.getResult(0);
    Value self = link(arith::AddIOp::create(rewriter, loc, slot, one));
    Value atFront = isNone(after);
    // (Where there is none before it, the links of the slot itself are
    // read and written in place of that one's: they are set below.)
    Value afterSlot = arith::SelectOp::create(
        rewriter, loc, atFront, slot,
        arith::SubIOp::create(rewriter, loc, world.toIndex(loc, after), one));
    Value next = arith::SelectOp::create(rewriter, loc, atFront,
                                         load(firstChild(), parent),
                                         load(nextSibling(), afterSlot));
    store(self, nextSibling(), afterSlot);
    store(arith::SelectOp::create(rewriter, loc, atFront, self,
                                  load(firstChild(), parent)),
          firstChild(), parent);
    store(after, previousSibling(), slot);
    store(next, nextSibling(), slot);
    Value atEnd = isNone(next);
    Value nextSlot = arith::SelectOp::create(
        rewriter, loc, atEnd, slot,
        arith::SubIOp::create(rewriter, loc, world.toIndex(loc, next), one));
    Value kept = load(previousSibling(), nextSlot);
    store(arith::SelectOp::create(rewriter, loc, atEnd, kept, self),
          previousSibling(), nextSlot);
    store(arith::SelectOp::create(rewriter, loc, atEnd, self,
                                  load(lastChild(), parent)),
          lastChild(), parent);
    store(arith::AddIOp::create(
              rewriter, loc, load(childCount(), parent),
              arith::ConstantIntOp::create(rewriter, loc, 1,
                                           relation.offsetBits)),
          childCount(), parent);
  }
  /// The entity `parent` gains or loses a child, which is an event where
  /// a reactive query has a trigger down the tree.
  void childrenChanged(Value parent) {
    if (relation.childTicksOffset) {
      store(world.currentTick(loc), world.childTicks(relation),
            world.entityKey(loc, parent));
      world.touch(loc, relation, parent);
    }
  }
  /// The slot `slot` leaves the children of the key its target has.
  void unlink(Value slot) {
    Value parent = world.entityKey(loc, load(targets(), slot));
    Value previous = load(previousSibling(), slot);
    Value next = load(nextSibling(), slot);
    // (The one after it has another sibling before it from here on, and
    // the one before it another after it.)
    if (relation.siblingTicksOffset)
      for (Value neighbour : {next, previous})
        branch(negate(isNone(neighbour)), [&] {
          Value beside = arith::SubIOp::create(
              rewriter, loc, world.toIndex(loc, neighbour), one);
          store(world.currentTick(loc), world.siblingTicks(relation), beside);
          world.touch(loc, relation, sourceOf(beside));
        });
    auto before = [&](Value linkValue) {
      return arith::SubIOp::create(rewriter, loc,
                                   world.toIndex(loc, linkValue), one)
          .getResult();
    };
    branch(
        isNone(previous), [&] { store(next, firstChild(), parent); },
        [&] { store(next, nextSibling(), before(previous)); });
    branch(
        isNone(next), [&] { store(previous, lastChild(), parent); },
        [&] { store(previous, previousSibling(), before(next)); });
    store(arith::SubIOp::create(
              rewriter, loc, load(childCount(), parent),
              arith::ConstantIntOp::create(rewriter, loc, 1,
                                           relation.offsetBits)),
          childCount(), parent);
  }
  /// The list's length, and putting `id` with its `parent` at its end,
  /// for the entity whose key is `key`.
  Value listed() { return load(world.treeOrderCount(relation), zero); }
  void list(Value id, Value parent, Value key) {
    Value length = listed();
    Value at = world.toIndex(loc, length);
    store(id, world.treeOrder(relation), at);
    store(parent, world.treeOrderParents(relation), at);
    if (relation.hasLocations()) {
      store(world.packedLocation(loc, id),
            world.treeOrderLocations(relation, /*parent=*/false), at);
      store(world.packedLocation(loc, parent),
            world.treeOrderLocations(relation, /*parent=*/true), at);
    }
    store(link(arith::AddIOp::create(rewriter, loc, at, one)), position(),
          key);
    store(arith::AddIOp::create(rewriter, loc, length, i64(1)),
          world.treeOrderCount(relation), zero);
  }
  void markUnclean() { store(i64(0), world.edgesClean(relation), zero); }
  /// The tree changes: an order worked out for a query is no more.
  void forgetWalk() {
    if (relation.walkStateOffset)
      store(i64(0), world.walkState(relation), zero);
  }

  /// The edge in `slot` is no more: out of the list, and one edge less.
  /// (Its place among its target's children is the caller's business.)
  void clearSlot(Value slot) {
    Value at = load(position(), slot);
    branch(negate(isNone(at)), [&] {
      store(world.noEntity(loc), world.treeOrder(relation),
            arith::SubIOp::create(rewriter, loc, world.toIndex(loc, at), one));
      store(noLink(), position(), slot);
    });
    store(noOwner(), sources(), slot);
    Value counter = world.edgeCount(relation);
    store(arith::SubIOp::create(rewriter, loc, load(counter, zero), i64(1)),
          counter, zero);
  }

  /// The entity of `slot` goes to the list's end: its entry, if it has
  /// one, becomes all ones, which a walk of the list skips. Without room
  /// at the end the relation is unclean, and the list made again.
  void relist(Value slot) {
    Value at = load(position(), slot);
    branch(negate(isNone(at)), [&] {
      store(world.noEntity(loc), world.treeOrder(relation),
            arith::SubIOp::create(rewriter, loc, world.toIndex(loc, at), one));
    });
    Value room = arith::CmpIOp::create(rewriter, loc,
                                       arith::CmpIPredicate::slt, listed(),
                                       i64(relation.orderCapacity));
    branch(
        room,
        [&] { list(sourceOf(slot), load(targets(), slot), slot); },
        [&] { markUnclean(); });
  }
  /// The entity of `slot`, which has a new parent that is in the list (or
  /// has no edge of its own), goes to the list's end with everything
  /// below it: itself, then the children of each entity moved, so every
  /// one is again after its parent. The end of the list is the queue.
  /// Coming to the entity itself again means its new parent was below it.
  void moveToEnd(Value slot) {
    Value from = world.toIndex(loc, listed());
    relist(slot);
    scf::WhileOp::create(
        rewriter, loc, TypeRange{rewriter.getIndexType()}, ValueRange{from},
        [&](OpBuilder &, Location, ValueRange state) {
          scf::ConditionOp::create(
              rewriter, loc,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                    state[0], world.toIndex(loc, listed())),
              state);
        },
        [&](OpBuilder &, Location, ValueRange state) {
          Value id = load(world.treeOrder(relation), state[0]);
          Value key = world.entityKey(loc, id);
          Value number = world.toIndex(loc, load(childCount(), key));
          Value first = world.toIndex(loc, load(firstChild(), key));
          auto children = scf::ForOp::create(rewriter, loc, zero, number, one,
                                             ValueRange{first});
          {
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(children.getBody());
            Value child = arith::SubIOp::create(
                rewriter, loc, children.getRegionIterArg(0), one);
            // (A slot a dead entity left in the list of a key that has a
            // new owner is not this entity's child.)
            branch(same(load(targets(), child), id), [&] {
              cf::AssertOp::create(
                  rewriter, loc, negate(same(child, slot)),
                  rewriter.getStringAttr(
                      "@" + RelationOp(relation.op).getSymName() +
                      " is a tree, but an entity is its own ancestor"));
              relist(child);
            });
            scf::YieldOp::create(
                rewriter, loc,
                ValueRange{world.toIndex(loc, load(nextSibling(), child))});
          }
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::AddIOp::create(rewriter, loc, state[0], one)});
        });
  }
};
} // namespace

static void assertEnds(IRRewriter &rewriter, Location loc,
                       const WorldLayout &layout, WorldAccess &world,
                       const WorldRelation &relation, Value source,
                       Value target);

/// Emit the function that connects an edge of a linked tree: (source,
/// target, the fields' values, the arena). The edge takes the slot of the
/// source's key, replacing the edge the source had (the last connect
/// wins) or one a dead entity left there, and joins the end of the
/// target's children. The tree's list stays in order, parents before
/// their children, where the entity is new to it and has no children (it
/// goes to the end) or is in it after its new parent (the parent's id is
/// changed in place); otherwise the relation is marked unclean, and built
/// again before anything reads it, which is also where a cycle is found.
static void emitConnectFunction(IRRewriter &rewriter, ModuleOp module,
                                const WorldLayout &layout,
                                const WorldRelation &relation,
                                MemRefType arenaType) {
  RelationOp relationOp = relation.op;
  Location loc = relationOp.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(module.getBody());
  Type idType = rewriter.getIntegerType(layout.entities.idBits);
  SmallVector<Type> inputs{idType, idType};
  for (const WorldColumn &field : relation.fields) {
    Type type = field.type;
    if (isa<EntityType>(type))
      type = idType;
    else if (auto text = dyn_cast<TextType>(type))
      type = text.getStorageType();
    else if (auto named = dyn_cast<EnumType>(type))
      type = named.getStorageType();
    inputs.push_back(type);
  }
  inputs.push_back(arenaType);
  auto func = func::FuncOp::create(rewriter, loc,
                                   connectFunctionName(relation),
                                   rewriter.getFunctionType(inputs, {}));
  func.setPrivate();
  Block *entry = func.addEntryBlock();
  rewriter.setInsertionPointToStart(entry);
  func::ReturnOp::create(rewriter, loc);
  rewriter.setInsertionPointToStart(entry);
  WorldAccess world(rewriter, layout, entry->getArguments().back());
  LinkedTree tree(rewriter, loc, world, relation);
  Value source = entry->getArgument(0), target = entry->getArgument(1);
  Value zero = tree.zero, one = tree.one;

  cf::AssertOp::create(
      rewriter, loc,
      arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne, source,
                            target),
      rewriter.getStringAttr("@" + relationOp.getSymName() +
                             " is a tree, but an entity is its own "
                             "ancestor"));
  Value slot = world.entityKey(loc, source);
  Value parentKey = world.entityKey(loc, target);
  tree.forgetWalk();
  // Among children with the same order, the one connected later is later.
  if (relation.isOrdered()) {
    Value numbers = world.connectNumbers(relation);
    Value count = arith::ConstantIndexOp::create(rewriter, loc,
                                                 layout.entityKeys);
    Value number = arith::AddIOp::create(rewriter, loc,
                                         tree.load(numbers, count),
                                         tree.i64(1));
    tree.store(number, numbers, count);
    tree.store(number, numbers, slot);
  }
  Value old = tree.load(tree.sources(), slot);
  Value isOwn = tree.same(old, world.slotOwner(loc, source));
  Value isNew = tree.same(old, tree.noOwner());
  Value isStale = tree.both(tree.negate(isOwn), tree.negate(isNew));
  // The edge a dead entity left in the slot goes, from its target's
  // children and from the list.
  tree.branch(isStale, [&] {
    tree.unlink(slot);
    Value at = tree.load(tree.position(), slot);
    tree.branch(tree.negate(tree.isNone(at)), [&] {
      tree.store(world.noEntity(loc), world.treeOrder(relation),
                 arith::SubIOp::create(rewriter, loc, world.toIndex(loc, at),
                                       one));
      tree.store(tree.noLink(), tree.position(), slot);
    });
  });
  tree.branch(isOwn, [&] {
    tree.childrenChanged(tree.load(tree.targets(), slot));
    tree.unlink(slot);
  });
  tree.childrenChanged(target);
  tree.branch(isNew, [&] {
    Value counter = world.edgeCount(relation);
    Value count = tree.load(counter, zero);
    cf::AssertOp::create(
        rewriter, loc,
        arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::slt, count,
                              tree.i64(relation.capacity)),
        rewriter.getStringAttr("ent.connect exceeds the capacity of @" +
                               relationOp.getSymName()));
    tree.store(arith::AddIOp::create(rewriter, loc, count, tree.i64(1)),
               counter, zero);
  });
  tree.store(world.slotOwner(loc, source), tree.sources(), slot);
  tree.store(target, tree.targets(), slot);
  for (auto [index, field] : llvm::enumerate(relation.fields))
    tree.store(entry->getArgument(2 + index),
               world.edgeField(relation, field), slot);
  if (relation.deadOffset)
    tree.store(arith::ConstantIntOp::create(rewriter, loc, 0, 8),
               world.edgeDead(relation), slot);
  tree.linkUnder(slot, parentKey);

  // The list. A parent that has no edge of its own is before everything;
  // one that has is where its position says.
  Value parentHas = tree.same(tree.load(tree.sources(), parentKey),
                              world.slotOwner(loc, target));
  Value parentAt = tree.load(tree.position(), parentKey);
  Value ownAt = tree.load(tree.position(), slot);
  Value parentListed = arith::OrIOp::create(
      rewriter, loc, tree.negate(parentHas),
      tree.negate(tree.isNone(parentAt)));
  // Where the list cannot take the edge with one store: the entity goes
  // to its end with all that is below it, if the list is in order and has
  // the parent; else it is to be made again anyway.
  auto moveOrMark = [&] {
    Value clean = tree.negate(
        tree.same(tree.load(world.edgesClean(relation), zero), tree.i64(0)));
    tree.branch(
        tree.both(clean, parentListed), [&] { tree.moveToEnd(slot); },
        [&] { tree.markUnclean(); });
  };
  tree.branch(
      isOwn,
      [&] {
        Value earlier = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::ult, parentAt, ownAt);
        Value inOrder = tree.both(
            tree.negate(tree.isNone(ownAt)),
            arith::OrIOp::create(
                rewriter, loc, tree.negate(parentHas),
                tree.both(tree.negate(tree.isNone(parentAt)), earlier)));
        tree.branch(
            inOrder,
            [&] {
              Value at = arith::SubIOp::create(
                  rewriter, loc, world.toIndex(loc, ownAt), one);
              tree.store(target, world.treeOrderParents(relation), at);
              if (relation.hasLocations())
                tree.store(
                    world.packedLocation(loc, target),
                    world.treeOrderLocations(relation, /*parent=*/true), at);
            },
            moveOrMark);
      },
      [&] {
        Value childless = tree.isNone(tree.load(tree.childCount(), slot));
        Value room = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::slt, tree.listed(),
            tree.i64(relation.orderCapacity));
        tree.branch(
            tree.both(childless, tree.both(parentListed, room)),
            [&] { tree.list(source, target, slot); }, moveOrMark);
      });
  // Where the children are in an order, the new one is put in its place
  // when the tree is next looked over.
  if (relation.isOrdered())
    tree.markUnclean();
}

/// Emit the function that takes a despawned entity out of a linked tree:
/// (its id, the arena). Its own edge leaves its parent's children, and
/// the edges to it go too, each child's from the child's slot: its
/// children are without a parent from here on, with all that is below
/// them as it was. So every edge of the tree has both its ends alive,
/// and neither is checked where the edges are followed.
static void emitDropFunction(IRRewriter &rewriter, ModuleOp module,
                             const WorldLayout &layout,
                             const WorldRelation &relation,
                             MemRefType arenaType) {
  RelationOp relationOp = relation.op;
  Location loc = relationOp.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(module.getBody());
  Type idType = rewriter.getIntegerType(layout.entities.idBits);
  auto func = func::FuncOp::create(
      rewriter, loc, dropFunctionName(relation),
      rewriter.getFunctionType({idType, arenaType}, {}));
  func.setPrivate();
  Block *entry = func.addEntryBlock();
  rewriter.setInsertionPointToStart(entry);
  func::ReturnOp::create(rewriter, loc);
  rewriter.setInsertionPointToStart(entry);
  WorldAccess world(rewriter, layout, entry->getArgument(1));
  LinkedTree tree(rewriter, loc, world, relation);
  Value id = entry->getArgument(0);
  Value slot = world.entityKey(loc, id);
  tree.forgetWalk();
  tree.branch(
      tree.same(tree.load(tree.sources(), slot), world.slotOwner(loc, id)),
      [&] {
        tree.childrenChanged(tree.load(tree.targets(), slot));
        tree.unlink(slot);
        tree.clearSlot(slot);
      });
  Value number = world.toIndex(loc, tree.load(tree.childCount(), slot));
  Value first = world.toIndex(loc, tree.load(tree.firstChild(), slot));
  auto children = scf::ForOp::create(rewriter, loc, tree.zero, number,
                                     tree.one, ValueRange{first});
  {
    OpBuilder::InsertionGuard inner(rewriter);
    rewriter.setInsertionPointToStart(children.getBody());
    Value child = arith::SubIOp::create(
        rewriter, loc, children.getRegionIterArg(0), tree.one);
    Value next = world.toIndex(loc, tree.load(tree.nextSibling(), child));
    tree.branch(
        tree.both(tree.negate(tree.same(tree.load(tree.sources(), child),
                                        tree.noOwner())),
                  tree.same(tree.load(tree.targets(), child), id)),
        [&] { tree.clearSlot(child); });
    scf::YieldOp::create(rewriter, loc, ValueRange{next});
  }
  for (Value column :
       {tree.firstChild(), tree.lastChild(), tree.childCount()})
    tree.store(tree.noLink(), column, slot);
}

/// Emit the function that, if a linked tree is unclean, builds it again
/// from its slots: the edges of dead entities, to dead entities and
/// disconnected ones go; every entity's children are linked in the order
/// of their keys; and the entities with a parent are listed by their
/// keys, each after those of its ancestors that were not in yet. An
/// entity on a cycle would wait for itself, which stops the program.
static void emitLinkedSortFunction(IRRewriter &rewriter, ModuleOp module,
                                   const WorldLayout &layout,
                                   const WorldRelation &relation,
                                   MemRefType arenaType) {
  RelationOp relationOp = relation.op;
  Location loc = relationOp.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(module.getBody());
  auto func = func::FuncOp::create(rewriter, loc, sortFunctionName(relation),
                                   rewriter.getFunctionType({arenaType}, {}));
  func.setPrivate();
  Block *entry = func.addEntryBlock();
  rewriter.setInsertionPointToStart(entry);
  func::ReturnOp::create(rewriter, loc);
  rewriter.setInsertionPointToStart(entry);
  WorldAccess world(rewriter, layout, entry->getArgument(0));
  LinkedTree tree(rewriter, loc, world, relation);
  Value zero = tree.zero, one = tree.one;
  Value keys = arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys);
  Value clean = tree.load(world.edgesClean(relation), zero);
  auto ifUnclean = scf::IfOp::create(rewriter, loc,
                                     tree.same(clean, tree.i64(0)));
  rewriter.setInsertionPointToStart(ifUnclean.thenBlock());
  tree.forgetWalk();

  auto hasEdge = [&](Value key) {
    return tree.negate(
        tree.same(tree.load(tree.sources(), key), tree.noOwner()));
  };
  // Which sibling each had before it, to tell afterwards who has another.
  if (relation.siblingTicksOffset)
    tree.forEach(zero, keys, [&](Value key) {
      tree.store(tree.load(tree.previousSibling(), key),
                 world.siblingsBefore(relation), key);
      tree.store(tree.load(tree.nextSibling(), key),
                 world.siblingsAfter(relation), key);
    });
  // The edges that stay, counted, and every link cleared.
  tree.store(tree.i64(0), world.edgeCount(relation), zero);
  tree.forEach(zero, keys, [&](Value key) {
    tree.branch(hasEdge(key), [&] {
      Value kept = tree.both(
          world.isAlive(loc, tree.sourceOf(key)),
          world.isAlive(loc, tree.load(tree.targets(), key)));
      if (relation.deadOffset)
        kept = tree.both(
            kept, tree.same(tree.load(world.edgeDead(relation), key),
                            arith::ConstantIntOp::create(rewriter, loc, 0, 8)));
      tree.branch(
          kept,
          [&] {
            Value counter = world.edgeCount(relation);
            tree.store(arith::AddIOp::create(rewriter, loc,
                                             tree.load(counter, zero),
                                             tree.i64(1)),
                       counter, zero);
          },
          [&] { tree.store(tree.noOwner(), tree.sources(), key); });
    });
    if (relation.deadOffset)
      tree.store(arith::ConstantIntOp::create(rewriter, loc, 0, 8),
                 world.edgeDead(relation), key);
    for (Value column : {tree.firstChild(), tree.lastChild(),
                         tree.childCount(), tree.position()})
      tree.store(tree.noLink(), column, key);
  });
  // Children, by their keys, or in their order where the tree has one
  // (what each is ordered by is read from it first, once).
  if (relation.isOrdered())
    tree.forEach(zero, keys, [&](Value key) {
      tree.branch(hasEdge(key), [&] {
        tree.store(tree.orderKey(layout, tree.sourceOf(key)),
                   world.orderKeys(relation), key);
      });
    });
  tree.forEach(zero, keys, [&](Value key) {
    tree.branch(hasEdge(key), [&] {
      Value parent = world.entityKey(loc, tree.load(tree.targets(), key));
      if (relation.isOrdered())
        tree.linkInOrder(layout, key, parent);
      else
        tree.linkUnder(key, parent);
    });
  });
  // The list of a tree whose children are in an order has them in it:
  // the children of the entities without a parent, then those of every
  // entity listed, each entity's as they are linked. An entity that is
  // its own ancestor is never come to.
  if (relation.isOrdered()) {
    tree.store(tree.i64(0), world.treeOrderCount(relation), zero);
    auto listChildren = [&](Value parent) {
      scf::WhileOp::create(
          rewriter, loc, TypeRange{world.offsetType(relation)},
          ValueRange{tree.load(tree.firstChild(), parent)},
          [&](OpBuilder &, Location, ValueRange state) {
            scf::ConditionOp::create(rewriter, loc,
                                     tree.negate(tree.isNone(state[0])),
                                     state);
          },
          [&](OpBuilder &, Location, ValueRange state) {
            Value child = arith::SubIOp::create(
                rewriter, loc, world.toIndex(loc, state[0]), one);
            tree.list(tree.sourceOf(child), tree.load(tree.targets(), child),
                      child);
            scf::YieldOp::create(
                rewriter, loc,
                ValueRange{tree.load(tree.nextSibling(), child)});
          });
    };
    tree.forEach(zero, keys, [&](Value key) {
      tree.branch(tree.negate(hasEdge(key)), [&] { listChildren(key); });
    });
    scf::WhileOp::create(
        rewriter, loc, TypeRange{rewriter.getIndexType()}, ValueRange{zero},
        [&](OpBuilder &, Location, ValueRange state) {
          scf::ConditionOp::create(
              rewriter, loc,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                    state[0],
                                    world.toIndex(loc, tree.listed())),
              state);
        },
        [&](OpBuilder &, Location, ValueRange state) {
          listChildren(world.entityKey(
              loc, tree.load(world.treeOrder(relation), state[0])));
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::AddIOp::create(rewriter, loc, state[0], one)
                             .getResult()});
        });
    cf::AssertOp::create(
        rewriter, loc,
        tree.same(tree.listed(), tree.load(world.edgeCount(relation), zero)),
        rewriter.getStringAttr("@" + relationOp.getSymName() +
                               " is a tree, but an entity is its own "
                               "ancestor"));
  }
  // The list, by key: an entity goes in once its parent is in (or has no
  // edge of its own). One whose parent is not is kept back with those of
  // its ancestors that are not either, the keys at the end of the list's
  // array, which has room for every entity that is yet to go in; then
  // they go in from the top. More kept back than are left to go in is a
  // cycle. Most parents are already in: a look at the parent's slot and
  // place, and no walk through the tree.
  Value order = world.treeOrder(relation);
  Value capacity =
      arith::ConstantIndexOp::create(rewriter, loc, relation.orderCapacity);
  Value edges =
      world.toIndex(loc, tree.load(world.edgeCount(relation), zero));
  if (!relation.isOrdered())
    tree.store(tree.i64(0), world.treeOrderCount(relation), zero);
  auto isIn = [&](Value key) {
    return tree.negate(tree.isNone(tree.load(tree.position(), key)));
  };
  Type index = rewriter.getIndexType();
  Type idType = world.idType();
  // (A tree whose children are in an order has its list: every entity is
  // in, and nothing is left for this.)
  tree.forEach(zero, relation.isOrdered() ? zero : keys, [&](Value key) {
    tree.branch(tree.both(hasEdge(key), tree.negate(isIn(key))), [&] {
      // (the key to keep back, where the kept start, whether to go on)
      auto climb = scf::WhileOp::create(
          rewriter, loc, TypeRange{index, index, rewriter.getI1Type()},
          ValueRange{key, capacity, tree.i1(true)},
          [&](OpBuilder &, Location, ValueRange state) {
            scf::ConditionOp::create(rewriter, loc, state[2], state);
          },
          [&](OpBuilder &, Location, ValueRange state) {
            Value top = arith::SubIOp::create(rewriter, loc, state[1], one);
            Value held = arith::SubIOp::create(rewriter, loc, capacity, top);
            Value left = arith::SubIOp::create(
                rewriter, loc, edges, world.toIndex(loc, tree.listed()));
            cf::AssertOp::create(
                rewriter, loc,
                arith::CmpIOp::create(rewriter, loc,
                                      arith::CmpIPredicate::sle, held, left),
                rewriter.getStringAttr("@" + relationOp.getSymName() +
                                       " is a tree, but an entity is its "
                                       "own ancestor"));
            tree.store(arith::IndexCastOp::create(rewriter, loc, idType,
                                                  state[0]),
                       order, top);
            Value parent = world.entityKey(
                loc, tree.load(tree.targets(), state[0]));
            Value waits =
                tree.both(hasEdge(parent), tree.negate(isIn(parent)));
            scf::YieldOp::create(rewriter, loc,
                                 ValueRange{parent, top, waits});
          });
      tree.forEach(climb.getResult(1), capacity, [&](Value at) {
        Value kept = arith::IndexCastUIOp::create(rewriter, loc, index,
                                                  tree.load(order, at));
        tree.list(tree.sourceOf(kept), tree.load(tree.targets(), kept), kept);
      });
    });
  });
  tree.store(tree.i64(1), world.edgesClean(relation), zero);
  // An entity with another sibling before it than it had: an event, where
  // a reactive query has a trigger before the tree.
  if (relation.siblingTicksOffset)
    tree.forEach(zero, keys, [&](Value key) {
      tree.branch(
          tree.both(
              hasEdge(key),
              tree.negate(tree.both(
                  tree.same(tree.load(tree.previousSibling(), key),
                            tree.load(world.siblingsBefore(relation), key)),
                  tree.same(tree.load(tree.nextSibling(), key),
                            tree.load(world.siblingsAfter(relation), key))))),
          [&] {
            tree.store(world.currentTick(loc), world.siblingTicks(relation),
                       key);
            world.touch(loc, relation, tree.sourceOf(key));
          });
    });
  if (!relation.hasLocations())
    return;
  // The list has the entities' and their parents' locations fresh from
  // being made. Otherwise, where rows have moved since they were noted,
  // they are read from the entity table again.
  tree.store(tree.i64(0), world.treeStale(relation), zero);
  rewriter.setInsertionPointAfter(ifUnclean);
  Value stale = tree.negate(
      tree.same(tree.load(world.treeStale(relation), zero), tree.i64(0)));
  tree.branch(stale, [&] {
    tree.forEach(zero, world.toIndex(loc, tree.listed()), [&](Value at) {
      Value id = tree.load(world.treeOrder(relation), at);
      tree.branch(tree.negate(tree.same(id, world.noEntity(loc))), [&] {
        tree.store(world.packedLocation(loc, id),
                   world.treeOrderLocations(relation, /*parent=*/false), at);
        tree.store(
            world.packedLocation(
                loc, tree.load(world.treeOrderParents(relation), at)),
            world.treeOrderLocations(relation, /*parent=*/true), at);
      });
    });
    tree.store(tree.i64(0), world.treeStale(relation), zero);
  });
}

/// Emit the function that, if `relation` is unclean, sorts its edges by
/// source with a stable counting sort through the scratch columns,
/// dropping dead edges and edges with an end that is no longer alive,
/// computes the offsets by source (and by target, with the positions of
/// those edges, where the program visits incoming edges) and marks it
/// clean. Edges keep the order they were connected in among those of the
/// same source, and the index by target lists each target's edges in table
/// order.
static void emitSortFunction(IRRewriter &rewriter, ModuleOp module,
                             const WorldLayout &layout,
                             const WorldRelation &relation,
                             MemRefType arenaType) {
  if (relation.linked) {
    emitLinkedSortFunction(rewriter, module, layout, relation, arenaType);
    emitConnectFunction(rewriter, module, layout, relation, arenaType);
    if (layout.entities.hasGenerations())
      emitDropFunction(rewriter, module, layout, relation, arenaType);
    return;
  }
  Location loc = RelationOp(relation.op).getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(module.getBody());
  auto func = func::FuncOp::create(rewriter, loc, sortFunctionName(relation),
                                   rewriter.getFunctionType({arenaType}, {}));
  func.setPrivate();
  Block *entry = func.addEntryBlock();
  rewriter.setInsertionPointToStart(entry);
  func::ReturnOp::create(rewriter, loc);
  rewriter.setInsertionPointToStart(entry);
  WorldAccess world(rewriter, layout, entry->getArgument(0));

  Type offsetType = world.offsetType(relation);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value keys = arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys);
  Value keysPlusOne =
      arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys + 1);
  Value clean = memref::LoadOp::create(rewriter, loc, world.edgesClean(relation),
                                       ValueRange{zero});
  auto ifUnclean = scf::IfOp::create(
      rewriter, loc,
      arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq, clean,
                            arith::ConstantIntOp::create(rewriter, loc, 0, 64)));
  rewriter.setInsertionPointToStart(ifUnclean.thenBlock());
  // (An order worked out for a query is no more.)
  if (relation.walkStateOffset)
    memref::StoreOp::create(
        rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
        world.walkState(relation), ValueRange{zero});

  auto forEach = [&](Value from, Value to,
                     function_ref<void(Value)> body) {
    auto loop = scf::ForOp::create(rewriter, loc, from, to, one);
    OpBuilder::InsertionGuard inner(rewriter);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    body(loop.getInductionVar());
  };
  auto offsetConstant = [&](int64_t value) {
    return arith::ConstantIntOp::create(rewriter, loc, value,
                                        relation.offsetBits);
  };
  // offsets[i] = 0; offsets[key + 1] += 1 per counted edge; prefix sums;
  // cursors = offsets.
  auto countInto = [&](Value offsets, Value edgeCount,
                       function_ref<Value(Value)> keyOf,
                       function_ref<Value(Value)> counts) {
    forEach(zero, keysPlusOne, [&](Value i) {
      memref::StoreOp::create(rewriter, loc, offsetConstant(0), offsets,
                              ValueRange{i});
    });
    forEach(zero, edgeCount, [&](Value k) {
      Value counted = counts(k);
      auto ifCounted = scf::IfOp::create(rewriter, loc, counted);
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(ifCounted.thenBlock());
      Value slot = arith::AddIOp::create(rewriter, loc, keyOf(k), one);
      Value old =
          memref::LoadOp::create(rewriter, loc, offsets, ValueRange{slot});
      memref::StoreOp::create(
          rewriter, loc,
          arith::AddIOp::create(rewriter, loc, old, offsetConstant(1)),
          offsets, ValueRange{slot});
    });
    forEach(one, keysPlusOne, [&](Value i) {
      Value previous = memref::LoadOp::create(
          rewriter, loc, offsets,
          ValueRange{arith::SubIOp::create(rewriter, loc, i, one)});
      Value own = memref::LoadOp::create(rewriter, loc, offsets, ValueRange{i});
      memref::StoreOp::create(
          rewriter, loc, arith::AddIOp::create(rewriter, loc, previous, own),
          offsets, ValueRange{i});
    });
    Value cursors = world.edgeCursors(relation);
    forEach(zero, keys, [&](Value i) {
      memref::StoreOp::create(
          rewriter, loc,
          memref::LoadOp::create(rewriter, loc, offsets, ValueRange{i}),
          cursors, ValueRange{i});
    });
  };
  // Take the cursor of `key` and advance it.
  auto take = [&](Value key) {
    Value cursors = world.edgeCursors(relation);
    Value position =
        memref::LoadOp::create(rewriter, loc, cursors, ValueRange{key});
    memref::StoreOp::create(
        rewriter, loc,
        arith::AddIOp::create(rewriter, loc, position, offsetConstant(1)),
        cursors, ValueRange{key});
    return position;
  };

  Value sources = world.edgeIds(relation, /*source=*/true);
  Value targets = world.edgeIds(relation, /*source=*/false);
  Value count = world.toIndex(
      loc, memref::LoadOp::create(rewriter, loc, world.edgeCount(relation),
                                  ValueRange{zero}));
  auto keep = [&](Value k) {
    Value source =
        memref::LoadOp::create(rewriter, loc, sources, ValueRange{k});
    Value target =
        memref::LoadOp::create(rewriter, loc, targets, ValueRange{k});
    Value kept = arith::AndIOp::create(rewriter, loc,
                                       world.isAlive(loc, source),
                                       world.isAlive(loc, target));
    if (relation.deadOffset) {
      Value dead = memref::LoadOp::create(rewriter, loc,
                                          world.edgeDead(relation),
                                          ValueRange{k});
      kept = arith::AndIOp::create(
          rewriter, loc, kept,
          arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq, dead,
                                arith::ConstantIntOp::create(rewriter, loc, 0,
                                                             8)));
    }
    return kept;
  };
  // The table's columns and their scratch copies.
  SmallVector<Value> table{sources, targets}, scratch{
      world.array(relation.sourceScratchOffset, relation.capacity,
                  world.idType()),
      world.array(relation.targetScratchOffset, relation.capacity,
                  world.idType())};
  for (auto [field, offset] :
       llvm::zip(relation.fields, relation.fieldScratchOffsets)) {
    table.push_back(world.edgeField(relation, field));
    scratch.push_back(world.array(offset, relation.capacity,
                                  world.storageType(field.type)));
  }
  auto keyIn = [&](ArrayRef<Value> columns, bool target) {
    return [&, columns, target](Value k) {
      return world.entityKey(
          loc, memref::LoadOp::create(rewriter, loc, columns[target ? 1 : 0],
                                      ValueRange{k}));
    };
  };
  auto all = [&](Value) {
    return arith::ConstantIntOp::create(rewriter, loc, 1, 1).getResult();
  };
  // Move the first `n` edges of `from` that `counted` keeps into `to`,
  // stably ordered by `keyOf`, through `offsets`.
  auto sortInto = [&](Value offsets, Value n, ArrayRef<Value> from,
                      ArrayRef<Value> to, function_ref<Value(Value)> keyOf,
                      function_ref<Value(Value)> counted) {
    countInto(offsets, n, keyOf, counted);
    forEach(zero, n, [&](Value k) {
      auto ifCounted = scf::IfOp::create(rewriter, loc, counted(k));
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(ifCounted.thenBlock());
      Value position = world.toIndex(loc, take(keyOf(k)));
      for (auto [source, target] : llvm::zip(from, to))
        memref::StoreOp::create(
            rewriter, loc,
            memref::LoadOp::create(rewriter, loc, source, ValueRange{k}),
            target, ValueRange{position});
    });
    return world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, offsets, ValueRange{keys}));
  };

  // By source into the scratch, dropping what is not kept; then back, by
  // source as it is or (stably, so still by source within a target) by
  // target.
  Value sorted = world.edgeOffsets(relation, /*in=*/relation.byTarget);
  Value kept;
  if (relation.tree) {
    // An entity has one parent, the target of the last edge connected
    // from it: note each source's last kept edge (its position and one, in
    // the cursors; 0 for none), then go through the keys in order, which
    // gives the offsets and the edges by source in one pass each. No
    // counting, and no second sort to drop the edges that lost.
    Value last = world.edgeCursors(relation);
    forEach(zero, keys, [&](Value key) {
      memref::StoreOp::create(rewriter, loc, offsetConstant(0), last,
                              ValueRange{key});
    });
    auto keyOfSource = keyIn(table, /*target=*/false);
    forEach(zero, count, [&](Value k) {
      auto ifKept = scf::IfOp::create(rewriter, loc, keep(k));
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(ifKept.thenBlock());
      memref::StoreOp::create(
          rewriter, loc,
          arith::IndexCastOp::create(
              rewriter, loc, offsetType,
              arith::AddIOp::create(rewriter, loc, k, one)),
          last, ValueRange{keyOfSource(k)});
    });
    auto place = scf::ForOp::create(rewriter, loc, zero, keys, one,
                                    ValueRange{zero});
    {
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(place.getBody());
      Value key = place.getInductionVar();
      Value at = place.getRegionIterArg(0);
      memref::StoreOp::create(
          rewriter, loc,
          arith::IndexCastOp::create(rewriter, loc, offsetType, at), sorted,
          ValueRange{key});
      Value edge = world.toIndex(
          loc, memref::LoadOp::create(rewriter, loc, last, ValueRange{key}));
      Value has = arith::CmpIOp::create(rewriter, loc,
                                        arith::CmpIPredicate::ne, edge, zero);
      auto ifHas = scf::IfOp::create(rewriter, loc, has);
      rewriter.setInsertionPointToStart(ifHas.thenBlock());
      Value from = arith::SubIOp::create(rewriter, loc, edge, one);
      for (auto [column, copy] : llvm::zip(table, scratch))
        memref::StoreOp::create(
            rewriter, loc,
            memref::LoadOp::create(rewriter, loc, column, ValueRange{from}),
            copy, ValueRange{at});
      rewriter.setInsertionPointAfter(ifHas);
      scf::YieldOp::create(
          rewriter, loc,
          ValueRange{arith::SelectOp::create(
              rewriter, loc, has, arith::AddIOp::create(rewriter, loc, at, one),
              at)});
    }
    kept = place.getResult(0);
    memref::StoreOp::create(
        rewriter, loc,
        arith::IndexCastOp::create(rewriter, loc, offsetType, kept), sorted,
        ValueRange{keys});
    forEach(zero, kept, [&](Value p) {
      for (auto [column, copy] : llvm::zip(table, scratch))
        memref::StoreOp::create(
            rewriter, loc,
            memref::LoadOp::create(rewriter, loc, copy, ValueRange{p}),
            column, ValueRange{p});
    });
  } else {
    kept = sortInto(sorted, count, table, scratch,
                    keyIn(table, /*target=*/false), keep);
    if (relation.byTarget) {
      sortInto(sorted, kept, scratch, table, keyIn(scratch, /*target=*/true),
               all);
    } else {
      forEach(zero, kept, [&](Value p) {
        for (auto [column, copy] : llvm::zip(table, scratch))
          memref::StoreOp::create(
              rewriter, loc,
              memref::LoadOp::create(rewriter, loc, copy, ValueRange{p}),
              column, ValueRange{p});
      });
    }
  }
  if (relation.deadOffset)
    forEach(zero, kept, [&](Value p) {
      memref::StoreOp::create(rewriter, loc,
                              arith::ConstantIntOp::create(rewriter, loc, 0, 8),
                              world.edgeDead(relation), ValueRange{p});
    });
  memref::StoreOp::create(
      rewriter, loc,
      arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(), kept),
      world.edgeCount(relation), ValueRange{zero});
  if (relation.tree)
    memref::StoreOp::create(
        rewriter, loc,
        arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(), kept),
        world.sortedCount(relation), ValueRange{zero});

  // Both ways: the index by target, listing each target's edges in table
  // order.
  if (relation.hasIndex()) {
    Value index = world.edgeOffsets(relation, /*in=*/true);
    auto keyOfTarget = keyIn(table, /*target=*/true);
    countInto(index, kept, keyOfTarget, all);
    forEach(zero, kept, [&](Value k) {
      Value position = world.toIndex(loc, take(keyOfTarget(k)));
      memref::StoreOp::create(
          rewriter, loc,
          arith::IndexCastOp::create(rewriter, loc, offsetType, k),
          world.indexEdges(relation), ValueRange{position});
    });
  }

  // A tree: list the entities with a parent, parents before children.
  // First the children of the entities without a parent, in table order,
  // then the children of each entity listed, as the index by target has
  // them. An entity on a cycle is never reached.
  if (relation.tree) {
    Value order = world.treeOrder(relation);
    Value length = world.treeOrderCount(relation);
    memref::StoreOp::create(rewriter, loc,
                            arith::ConstantIntOp::create(rewriter, loc, 0, 64),
                            length, ValueRange{zero});
    Value orderParents = world.treeOrderParents(relation);
    LinkedTree ordering(rewriter, loc, world, relation);
    auto list = [&](Value id, Value parent) {
      // (What it is ordered by, read once: see inOrder.)
      if (relation.isOrdered())
        memref::StoreOp::create(rewriter, loc,
                                ordering.orderKey(layout, id),
                                world.orderKeys(relation),
                                ValueRange{world.entityKey(loc, id)});
      Value n = memref::LoadOp::create(rewriter, loc, length, ValueRange{zero});
      memref::StoreOp::create(rewriter, loc, id, order,
                              ValueRange{world.toIndex(loc, n)});
      memref::StoreOp::create(rewriter, loc, parent, orderParents,
                              ValueRange{world.toIndex(loc, n)});
      memref::StoreOp::create(
          rewriter, loc,
          arith::AddIOp::create(
              rewriter, loc, n,
              arith::ConstantIntOp::create(rewriter, loc, 1, 64)),
          length, ValueRange{zero});
    };
    auto listed = [&]() -> Value {
      return world.toIndex(loc, memref::LoadOp::create(rewriter, loc, length,
                                                       ValueRange{zero}));
    };
    // The children of one entity, the list's elements from `from` on, in
    // their order where the tree has one: by what they are ordered by,
    // then by their connects. Each is put in its place among those before
    // it, which it is in already if they came in order.
    auto inOrder = [&](Value from) {
      if (!relation.isOrdered())
        return;
      Value keys = world.orderKeys(relation);
      Value numbers = world.connectNumbers(relation);
      Type index = rewriter.getIndexType();
      forEach(from, listed(), [&](Value at) {
        Value id = memref::LoadOp::create(rewriter, loc, order, ValueRange{at});
        Value key = world.entityKey(loc, id);
        Value own = memref::LoadOp::create(rewriter, loc, keys, ValueRange{key});
        Value number =
            memref::LoadOp::create(rewriter, loc, numbers, ValueRange{key});
        auto back = scf::WhileOp::create(
            rewriter, loc, TypeRange{index}, ValueRange{at},
            [&](OpBuilder &, Location, ValueRange state) {
              // On while there is one before here, and it comes after.
              Value any = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::ugt, state[0], from);
              auto ifAny = scf::IfOp::create(rewriter, loc,
                                             TypeRange{rewriter.getI1Type()},
                                             any, /*withElseRegion=*/true);
              {
                OpBuilder::InsertionGuard inner(rewriter);
                rewriter.setInsertionPointToStart(ifAny.thenBlock());
                Value other = world.entityKey(
                    loc, memref::LoadOp::create(
                             rewriter, loc, order,
                             ValueRange{arith::SubIOp::create(
                                            rewriter, loc, state[0], one)
                                            .getResult()}));
                Value otherKey = memref::LoadOp::create(rewriter, loc, keys,
                                                        ValueRange{other});
                scf::YieldOp::create(
                    rewriter, loc,
                    ValueRange{arith::OrIOp::create(
                        rewriter, loc,
                        arith::CmpIOp::create(rewriter, loc,
                                              arith::CmpIPredicate::sgt,
                                              otherKey, own),
                        ordering.both(
                            ordering.same(otherKey, own),
                            arith::CmpIOp::create(
                                rewriter, loc, arith::CmpIPredicate::sgt,
                                memref::LoadOp::create(rewriter, loc, numbers,
                                                       ValueRange{other}),
                                number)))});
                rewriter.setInsertionPointToStart(ifAny.elseBlock());
                scf::YieldOp::create(rewriter, loc,
                                     ValueRange{ordering.i1(false)});
              }
              scf::ConditionOp::create(rewriter, loc, ifAny.getResult(0),
                                       state);
            },
            [&](OpBuilder &, Location, ValueRange state) {
              Value before =
                  arith::SubIOp::create(rewriter, loc, state[0], one);
              memref::StoreOp::create(
                  rewriter, loc,
                  memref::LoadOp::create(rewriter, loc, order,
                                         ValueRange{before}),
                  order, ValueRange{state[0]});
              scf::YieldOp::create(rewriter, loc, ValueRange{before});
            });
        memref::StoreOp::create(rewriter, loc, id, order,
                                ValueRange{back.getResult(0)});
      });
    };
    // (The entities without a parent are taken archetype by archetype in
    // the order of their rows, which is the order they keep: so the
    // children of one parent are next to each other all the way down,
    // and the parents of what is next to each other are in order.)
    for (unsigned index : relation.sortedArchetypes) {
      const WorldArchetype &archetype = layout.archetypes[index];
      Value ids = world.ids(archetype);
      forEach(zero, world.count(loc, archetype), [&](Value row) {
        Value id = memref::LoadOp::create(rewriter, loc, ids, ValueRange{row});
        Value hasParent =
            emitParent(rewriter, loc, layout, world, relation, id).first;
        auto ifRoot = scf::IfOp::create(
            rewriter, loc,
            arith::XOrIOp::create(
                rewriter, loc, hasParent,
                arith::ConstantIntOp::create(rewriter, loc, 1, 1)));
        OpBuilder::InsertionGuard inner(rewriter);
        rewriter.setInsertionPointToStart(ifRoot.thenBlock());
        auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                          /*in=*/true,
                                          world.entityKey(loc, id));
        Value first = listed();
        forEach(begin, end, [&](Value position) {
          Value edge = world.toIndex(
              loc, memref::LoadOp::create(rewriter, loc,
                                          world.indexEdges(relation),
                                          ValueRange{position}));
          list(memref::LoadOp::create(rewriter, loc, sources,
                                      ValueRange{edge}),
               id);
        });
        inOrder(first);
      });
    }
    scf::WhileOp::create(
        rewriter, loc, TypeRange{rewriter.getIndexType()}, ValueRange{zero},
        [&](OpBuilder &, Location, ValueRange state) {
          Value n = world.toIndex(
              loc, memref::LoadOp::create(rewriter, loc, length,
                                          ValueRange{zero}));
          scf::ConditionOp::create(
              rewriter, loc,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                    state[0], n),
              state);
        },
        [&](OpBuilder &, Location, ValueRange state) {
          Value id = memref::LoadOp::create(rewriter, loc, order,
                                            ValueRange{state[0]});
          auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                            /*in=*/true,
                                            world.entityKey(loc, id));
          Value first = listed();
          forEach(begin, end, [&](Value position) {
            Value edge = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc,
                                            world.indexEdges(relation),
                                            ValueRange{position}));
            list(memref::LoadOp::create(rewriter, loc, sources,
                                        ValueRange{edge}),
                 id);
          });
          inOrder(first);
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::AddIOp::create(rewriter, loc, state[0], one)});
        });
    Value listedInAll =
        memref::LoadOp::create(rewriter, loc, length, ValueRange{zero});
    cf::AssertOp::create(
        rewriter, loc,
        arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq, listedInAll,
            arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                       kept)),
        rewriter.getStringAttr("@" + RelationOp(relation.op).getSymName() +
                               " is a tree, but an entity is its own "
                               "ancestor"));

    // The archetypes the tree keeps in its order: their rows into it. The
    // entities without a parent first, as they are, then the others as
    // listed, which is by depth; every column and the ids move, the entity
    // table follows, and each row notes where its parent is. A tree in
    // several archetypes also notes, per archetype, where each depth
    // starts.
    if (!relation.sortedArchetypes.empty()) {
      SmallVector<const WorldArchetype *> holders;
      for (unsigned index : relation.sortedArchetypes)
        holders.push_back(&layout.archetypes[index]);
      bool alone = holders.size() == 1;
      Type i32 = rewriter.getI32Type();
      auto asRow = [&](Value index) -> Value {
        return arith::IndexCastOp::create(rewriter, loc, i32, index);
      };
      auto newRowsOf = [&](const WorldArchetype &archetype) {
        return world.array(archetype.newRowOffset, archetype.capacity, i32);
      };
      unsigned whereBits = layout.entities.locationBits;
      auto isIn = [&](Value where, const WorldArchetype &archetype) -> Value {
        return arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq, where,
            arith::ConstantIntOp::create(rewriter, loc, archetype.index,
                                         whereBits));
      };
      SmallVector<Value> roots;
      for (const WorldArchetype *archetype : holders) {
        Value rows = world.count(loc, *archetype);
        Value ids = world.ids(*archetype);
        Value newRows = newRowsOf(*archetype);
        auto unparented = scf::ForOp::create(rewriter, loc, zero, rows, one,
                                             ValueRange{zero});
        {
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(unparented.getBody());
          Value row = unparented.getInductionVar();
          Value at = unparented.getRegionIterArg(0);
          Value hasParent =
              emitParent(rewriter, loc, layout, world, relation,
                         memref::LoadOp::create(rewriter, loc, ids,
                                                ValueRange{row}))
                  .first;
          auto ifRoot = scf::IfOp::create(
              rewriter, loc,
              arith::XOrIOp::create(
                  rewriter, loc, hasParent,
                  arith::ConstantIntOp::create(rewriter, loc, 1, 1)));
          rewriter.setInsertionPointToStart(ifRoot.thenBlock());
          memref::StoreOp::create(rewriter, loc, asRow(at), newRows,
                                  ValueRange{row});
          rewriter.setInsertionPointAfter(ifRoot);
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::SelectOp::create(
                  rewriter, loc, hasParent, at,
                  arith::AddIOp::create(rewriter, loc, at, one))});
        }
        roots.push_back(unparented.getResult(0));
        memref::StoreOp::create(
            rewriter, loc,
            arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                       roots.back()),
            world.rootCount(*archetype), ValueRange{zero});
      }
      Value entries = world.toIndex(loc, listedInAll);
      auto locationOf = [&](Value id) {
        return world.getLocation(loc, world.idSlot(loc, id));
      };
      {
        // The list is by depth: an entity's is its parent's and one, kept
        // per entity in the sort's cursors, which are free now. Each goes
        // to the next row of its archetype, and where the depth moves on,
        // every archetype notes its next row.
        Value depths = world.edgeCursors(relation);
        SmallVector<Value> inits{zero};
        inits.append(roots.begin(), roots.end());
        auto place = scf::ForOp::create(rewriter, loc, zero, entries, one,
                                        inits);
        {
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(place.getBody());
          Value k = place.getInductionVar();
          Value previous = place.getRegionIterArg(0);
          Value id =
              memref::LoadOp::create(rewriter, loc, order, ValueRange{k});
          Value parentKey = world.entityKey(
              loc, memref::LoadOp::create(rewriter, loc, orderParents,
                                          ValueRange{k}));
          auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                            /*in=*/false, parentKey);
          Value parentDepth = arith::SelectOp::create(
              rewriter, loc,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                    begin, end),
              zero,
              world.toIndex(loc, memref::LoadOp::create(
                                     rewriter, loc, depths,
                                     ValueRange{parentKey})));
          Value depth = arith::AddIOp::create(rewriter, loc, parentDepth, one);
          memref::StoreOp::create(
              rewriter, loc,
              arith::IndexCastOp::create(rewriter, loc, offsetType, depth),
              depths, ValueRange{world.entityKey(loc, id)});
          auto ifDeeper = scf::IfOp::create(
              rewriter, loc,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                    depth, previous));
          {
            OpBuilder::InsertionGuard deeper(rewriter);
            rewriter.setInsertionPointToStart(ifDeeper.thenBlock());
            for (auto [index, archetype] : llvm::enumerate(holders))
              memref::StoreOp::create(
                  rewriter, loc, asRow(place.getRegionIterArg(1 + index)),
                  world.levelStarts(*archetype), ValueRange{depth});
          }
          auto [where, row] = locationOf(id);
          SmallVector<Value> next{depth};
          for (auto [index, archetype] : llvm::enumerate(holders)) {
            Value at = place.getRegionIterArg(1 + index);
            Value here = isIn(where, *archetype);
            auto ifHere = scf::IfOp::create(rewriter, loc, here);
            {
              OpBuilder::InsertionGuard guard(rewriter);
              rewriter.setInsertionPointToStart(ifHere.thenBlock());
              memref::StoreOp::create(rewriter, loc, asRow(at),
                                      newRowsOf(*archetype), ValueRange{row});
            }
            next.push_back(arith::SelectOp::create(
                rewriter, loc, here,
                arith::AddIOp::create(rewriter, loc, at, one), at));
          }
          scf::YieldOp::create(rewriter, loc, next);
        }
        Value depth = place.getResult(0);
        memref::StoreOp::create(
            rewriter, loc,
            arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                       depth),
            world.treeDepth(relation), ValueRange{zero});
        Value past = arith::AddIOp::create(rewriter, loc, depth, one);
        for (auto [index, archetype] : llvm::enumerate(holders))
          memref::StoreOp::create(rewriter, loc,
                                  asRow(place.getResult(1 + index)),
                                  world.levelStarts(*archetype),
                                  ValueRange{past});
        // Depth 0, the entities without a parent, starts at row 0.
        for (const WorldArchetype *archetype : holders)
          memref::StoreOp::create(rewriter, loc, asRow(zero),
                                  world.levelStarts(*archetype),
                                  ValueRange{zero});
      }
      for (const WorldArchetype *archetype : holders) {
        Value rows = world.count(loc, *archetype);
        Value ids = world.ids(*archetype);
        Value newRows = newRowsOf(*archetype);
        SmallVector<std::pair<Value, Value>> columns;
        for (auto [column, offset] :
             llvm::zip(archetype->columns, archetype->columnScratchOffsets))
          columns.push_back({world.moveValues(*archetype, column),
                             world.array(offset, archetype->capacity,
                                         world.storageType(column.type))});
        columns.push_back(
            {ids, world.array(archetype->idScratchOffset, archetype->capacity,
                              world.idType())});
        for (auto [column, copy] : columns) {
          forEach(zero, rows, [&, column = column, copy = copy](Value row) {
            memref::StoreOp::create(
                rewriter, loc,
                memref::LoadOp::create(rewriter, loc, column, ValueRange{row}),
                copy, ValueRange{row});
          });
          forEach(zero, rows, [&, column = column, copy = copy](Value row) {
            Value to = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc, newRows,
                                            ValueRange{row}));
            memref::StoreOp::create(
                rewriter, loc,
                memref::LoadOp::create(rewriter, loc, copy, ValueRange{row}),
                column, ValueRange{to});
          });
        }
        forEach(zero, rows, [&](Value row) {
          Value id =
              memref::LoadOp::create(rewriter, loc, ids, ValueRange{row});
          world.setLocation(loc, world.idSlot(loc, id), *archetype, row);
        });
        // Its rows have moved: another tree that keeps a slot for its
        // entities has their locations to read again.
        for (const WorldRelation &other : layout.relations)
          if (other.hasLocations() && canHoldEnd(*archetype, other)) {
            memref::StoreOp::create(
                rewriter, loc,
                arith::ConstantIntOp::create(rewriter, loc, 1, 64),
                world.treeStale(other), ValueRange{zero});
            callSort(rewriter, loc, other, world.getArena());
          }
      }
      if (alone) {
        Value parentRows = world.parentRows(*holders[0]);
        forEach(zero, roots[0], [&](Value row) {
          memref::StoreOp::create(
              rewriter, loc,
              arith::ConstantIntOp::create(rewriter, loc, -1, 32), parentRows,
              ValueRange{row});
        });
        forEach(zero, entries, [&](Value k) {
          Value parent = memref::LoadOp::create(rewriter, loc, orderParents,
                                                ValueRange{k});
          memref::StoreOp::create(
              rewriter, loc, asRow(locationOf(parent).second), parentRows,
              ValueRange{arith::AddIOp::create(rewriter, loc, roots[0], k)});
        });
        // And every row the rows of its children, which are next to each
        // other: where a row's parent is another than the row's before,
        // the parent's children begin, and the one before's have ended.
        const WorldArchetype &only = *holders[0];
        Value rows = world.count(loc, only);
        Value begins = world.childRows(only, /*begin=*/true);
        Value ends = world.childRows(only, /*begin=*/false);
        Value none = arith::ConstantIntOp::create(rewriter, loc, 0, 32);
        forEach(zero, rows, [&](Value row) {
          memref::StoreOp::create(rewriter, loc, none, begins,
                                  ValueRange{row});
          memref::StoreOp::create(rewriter, loc, none, ends, ValueRange{row});
        });
        forEach(roots[0], rows, [&](Value row) {
          Value parent = world.toIndex(
              loc, memref::LoadOp::create(rewriter, loc, parentRows,
                                          ValueRange{row}));
          Value isFirst = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::eq, row, roots[0]);
          Value before = world.toIndex(
              loc, memref::LoadOp::create(
                       rewriter, loc, parentRows,
                       ValueRange{arith::SelectOp::create(
                           rewriter, loc, isFirst, row,
                           arith::SubIOp::create(rewriter, loc, row, one))}));
          Value starts = arith::OrIOp::create(
              rewriter, loc, isFirst,
              arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne,
                                    parent, before));
          auto ifStarts = scf::IfOp::create(rewriter, loc, starts);
          {
            OpBuilder::InsertionGuard inner(rewriter);
            rewriter.setInsertionPointToStart(ifStarts.thenBlock());
            memref::StoreOp::create(rewriter, loc, asRow(row), begins,
                                    ValueRange{parent});
          }
          // The last of its children so far: the end moves on with them.
          memref::StoreOp::create(
              rewriter, loc,
              asRow(arith::AddIOp::create(rewriter, loc, row, one)), ends,
              ValueRange{parent});
        });
      } else {
        // (And, for a query that follows events down the tree, every row
        // the rows of its children in each archetype: they are next to
        // each other there, the list having an entity's children one
        // after another.)
        // (An entity without a parent has no sibling before it or after
        // it; those with one are given theirs below.)
        for (const WorldArchetype *archetype : holders)
          if (archetype->beforeIdOffset)
            forEach(zero, world.count(loc, *archetype), [&](Value row) {
              for (Value ids : {world.beforeIds(*archetype),
                                world.afterIds(*archetype)})
                memref::StoreOp::create(rewriter, loc, world.noEntity(loc),
                                        ids, ValueRange{row});
            });
        bool ranges = !holders[0]->childRangeOffsets.empty();
        if (ranges)
          for (const WorldArchetype *archetype : holders)
            forEach(zero, world.count(loc, *archetype), [&](Value row) {
              for (unsigned holder = 0; holder < holders.size(); ++holder)
                for (bool begin : {true, false})
                  memref::StoreOp::create(
                      rewriter, loc,
                      arith::ConstantIntOp::create(rewriter, loc, 0, 32),
                      world.childRange(*archetype, holder, begin),
                      ValueRange{row});
            });
        forEach(zero, entries, [&](Value k) {
          Value id =
              memref::LoadOp::create(rewriter, loc, order, ValueRange{k});
          Value parent = memref::LoadOp::create(rewriter, loc, orderParents,
                                                ValueRange{k});
          Value packed = memref::LoadOp::create(
              rewriter, loc, world.locations(),
              ValueRange{world.idSlot(loc, parent)});
          auto [where, row] = locationOf(id);
          auto [parentWhere, parentRow] = locationOf(parent);
          // (Both also next to the entity in the list, for going through
          // a deep tree by it.)
          memref::StoreOp::create(
              rewriter, loc,
              memref::LoadOp::create(rewriter, loc, world.locations(),
                                     ValueRange{world.idSlot(loc, id)}),
              world.rowOrder(relation, /*parent=*/false), ValueRange{k});
          memref::StoreOp::create(rewriter, loc, packed,
                                  world.rowOrder(relation, /*parent=*/true),
                                  ValueRange{k});
          for (auto [index, archetype] : llvm::enumerate(holders)) {
            auto ifHere =
                scf::IfOp::create(rewriter, loc, isIn(where, *archetype));
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(ifHere.thenBlock());
            memref::StoreOp::create(rewriter, loc, packed,
                                    world.parentLocations(*archetype),
                                    ValueRange{row});
            // (The sibling before: the one before in the list, if that
            // has the same parent.)
            // (And the one after: the next in the list, likewise.)
            if (archetype->afterIdOffset) {
              Value next = arith::AddIOp::create(rewriter, loc, k, one);
              Value any = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::ult, next, entries);
              Value at = arith::SelectOp::create(rewriter, loc, any, next, k);
              Value same = arith::AndIOp::create(
                  rewriter, loc, any,
                  arith::CmpIOp::create(
                      rewriter, loc, arith::CmpIPredicate::eq,
                      memref::LoadOp::create(rewriter, loc, orderParents,
                                             ValueRange{at}),
                      parent));
              memref::StoreOp::create(
                  rewriter, loc,
                  arith::SelectOp::create(
                      rewriter, loc, same,
                      memref::LoadOp::create(rewriter, loc, order,
                                             ValueRange{at}),
                      world.noEntity(loc)),
                  world.afterIds(*archetype), ValueRange{row});
            }
            if (archetype->beforeIdOffset) {
              Value any = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::ugt, k, zero);
              Value last = arith::SelectOp::create(
                  rewriter, loc, any,
                  arith::SubIOp::create(rewriter, loc, k, one), k);
              Value same = arith::AndIOp::create(
                  rewriter, loc, any,
                  arith::CmpIOp::create(
                      rewriter, loc, arith::CmpIPredicate::eq,
                      memref::LoadOp::create(rewriter, loc, orderParents,
                                             ValueRange{last}),
                      parent));
              memref::StoreOp::create(
                  rewriter, loc,
                  arith::SelectOp::create(
                      rewriter, loc, same,
                      memref::LoadOp::create(rewriter, loc, order,
                                             ValueRange{last}),
                      world.noEntity(loc)),
                  world.beforeIds(*archetype), ValueRange{row});
            }
            if (!ranges)
              continue;
            for (const WorldArchetype *home : holders) {
              auto ifThere = scf::IfOp::create(rewriter, loc,
                                               isIn(parentWhere, *home));
              OpBuilder::InsertionGuard inner(rewriter);
              rewriter.setInsertionPointToStart(ifThere.thenBlock());
              Value begins = world.childRange(*home, index, /*begin=*/true);
              Value ends = world.childRange(*home, index, /*begin=*/false);
              Value none = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq,
                  memref::LoadOp::create(rewriter, loc, ends,
                                         ValueRange{parentRow}),
                  arith::ConstantIntOp::create(rewriter, loc, 0, 32));
              memref::StoreOp::create(
                  rewriter, loc,
                  arith::SelectOp::create(
                      rewriter, loc, none, asRow(row),
                      memref::LoadOp::create(rewriter, loc, begins,
                                             ValueRange{parentRow})),
                  begins, ValueRange{parentRow});
              memref::StoreOp::create(
                  rewriter, loc,
                  asRow(arith::AddIOp::create(rewriter, loc, row, one)), ends,
                  ValueRange{parentRow});
            }
          }
        });
      }
      // And each row when its entity was last connected, where a reactive
      // query asks (the relation has it by the entity's key).
      for (const WorldArchetype *archetype : holders)
        if (archetype->connectedRowOffset)
          forEach(zero, world.count(loc, *archetype), [&](Value row) {
            memref::StoreOp::create(
                rewriter, loc,
                memref::LoadOp::create(
                    rewriter, loc, world.connectedTicks(relation),
                    ValueRange{world.entityKey(
                        loc, world.entityId(loc, *archetype, row))}),
                world.connectedRows(*archetype), ValueRange{row});
          });
    }
  }
  // Who has other children than when the rows were last put in order, and
  // who other siblings: events, where a reactive query has a trigger down
  // the tree or before or after it.
  auto each = [&](Value from, Value to, function_ref<void(Value)> body) {
    auto loop = scf::ForOp::create(rewriter, loc, from, to, one);
    OpBuilder::InsertionGuard inner(rewriter);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    body(loop.getInductionVar());
  };
  auto when = [&](Value condition, function_ref<void()> then) {
    auto ifOp = scf::IfOp::create(rewriter, loc, condition);
    OpBuilder::InsertionGuard inner(rewriter);
    rewriter.setInsertionPointToStart(ifOp.thenBlock());
    then();
  };
  auto at = [&](Value column, Value index) -> Value {
    return memref::LoadOp::create(rewriter, loc, column, ValueRange{index});
  };
  auto differs = [&](Value a, Value b) -> Value {
    return arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne, a,
                                 b);
  };
  if (relation.childCountsOffset) {
    FlatSymbolRefAttr has = RelationOp(relation.op).getEndpoint(/*target=*/true);
    for (const WorldArchetype &archetype : layout.archetypes) {
      if (has && !ArchetypeOp(archetype.op).contains(has))
        continue;
      each(zero, world.count(loc, archetype), [&](Value row) {
        Value id = world.entityId(loc, archetype, row);
        Value key = world.entityKey(loc, id);
        auto [begin, end] =
            emitEdgeRange(rewriter, loc, world, relation, /*in=*/true, key);
        Value number = arith::IndexCastOp::create(
            rewriter, loc, rewriter.getI64Type(),
            arith::SubIOp::create(rewriter, loc, end, begin));
        when(differs(number, at(world.childCounts(relation), key)), [&] {
          memref::StoreOp::create(rewriter, loc, number,
                                  world.childCounts(relation),
                                  ValueRange{key});
          memref::StoreOp::create(rewriter, loc, world.currentTick(loc),
                                  world.childTicks(relation), ValueRange{key});
          world.touch(loc, relation, id);
        });
      });
    }
  }
  if (relation.siblingIdsBeforeOffset) {
    bool single = relation.sortedArchetype() >= 0;
    for (unsigned index : relation.sortedArchetypes) {
      const WorldArchetype &archetype = layout.archetypes[index];
      Value rows = world.count(loc, archetype);
      Value roots = world.toIndex(loc, at(world.rootCount(archetype), zero));
      each(zero, rows, [&](Value row) {
        Value id = world.entityId(loc, archetype, row);
        Value key = world.entityKey(loc, id);
        Value changed;
        for (bool after : {false, true}) {
          Value sibling;
          if (single) {
            // The row before or after, if it has the same parent.
            Value next = arith::AddIOp::create(rewriter, loc, row, one);
            Value there =
                after ? arith::AndIOp::create(
                            rewriter, loc,
                            arith::CmpIOp::create(rewriter, loc,
                                                  arith::CmpIPredicate::uge,
                                                  row, roots),
                            arith::CmpIOp::create(rewriter, loc,
                                                  arith::CmpIPredicate::ult,
                                                  next, rows))
                            .getResult()
                      : arith::CmpIOp::create(rewriter, loc,
                                              arith::CmpIPredicate::ugt, row,
                                              roots)
                            .getResult();
            Value other = arith::SelectOp::create(
                rewriter, loc, there,
                after ? next
                      : arith::SubIOp::create(rewriter, loc, row, one)
                            .getResult(),
                row);
            Value parents = world.parentRows(archetype);
            Value same = arith::AndIOp::create(
                rewriter, loc, there,
                arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                      at(parents, other), at(parents, row)));
            sibling = arith::SelectOp::create(
                rewriter, loc, same, world.entityId(loc, archetype, other),
                world.noEntity(loc));
          } else {
            sibling = at(after ? world.afterIds(archetype)
                               : world.beforeIds(archetype),
                         row);
          }
          Value had = world.siblingIds(relation, after);
          Value other = differs(sibling, at(had, key));
          memref::StoreOp::create(rewriter, loc, sibling, had,
                                  ValueRange{key});
          changed = changed ? arith::OrIOp::create(rewriter, loc, changed,
                                                   other)
                                  .getResult()
                            : other;
        }
        when(changed, [&] {
          memref::StoreOp::create(rewriter, loc, world.currentTick(loc),
                                  world.siblingTicks(relation),
                                  ValueRange{key});
          world.touch(loc, relation, id);
        });
      });
    }
  }
  memref::StoreOp::create(rewriter, loc,
                          arith::ConstantIntOp::create(rewriter, loc, 1, 64),
                          world.edgesClean(relation), ValueRange{zero});
}

/// Lower the connects left in `func`, those outside queries: append the
/// edge. The caller has the relation sorted before the system's next
/// query and when the system ends, so the edge is there for whatever can
/// see it, and a loop of connects sorts once.
static void lowerConnects(IRRewriter &rewriter, func::FuncOp func,
                          const WorldLayout &layout, WorldAccess &world) {
  SmallVector<ConnectOp> connects;
  func.walk([&](ConnectOp connect) { connects.push_back(connect); });
  for (ConnectOp connect : connects) {
    Location loc = connect.getLoc();
    rewriter.setInsertionPoint(connect);
    const WorldRelation &relation =
        layout.getRelation(connect.getRelationAttr().getAttr());
    SmallVector<Value> values;
    for (Value value : connect.getValues())
      values.push_back(world.toStorage(loc, value));
    appendEdge(rewriter, loc, layout, world, relation,
               world.toStorage(loc, connect.getSource()),
               world.toStorage(loc, connect.getTarget()), values);
    rewriter.eraseOp(connect);
  }
}

/// Why the reactive `query` scans every entity on each run instead of
/// walking its triggers' event logs, or nothing if it can walk them. The
/// log is in the order events happened, not in row order: queries that
/// combine applies or apply pending structural changes rely on row order.
static std::optional<std::string> whyScans(QueryOp query,
                                           const WorldLayout &layout) {
  for (const Trigger &trigger : getTriggers(query)) {
    if (trigger.via)
      return std::string("it reacts to events of an ancestor, which is "
                         "found from each entity");
    if (!layout.findLog(getStamp(trigger)))
      return "the event log of a trigger has capacity 0";
  }
  bool applies = false, spawns = false;
  query.getBody().walk([&](Operation *op) {
    applies |= isa<ApplyOp, AccumulateOp, ConnectOp>(op);
    spawns |= isa<SpawnOp>(op);
  });
  if (applies)
    return std::string("it applies or accumulates values or connects "
                       "edges, which are combined in row order");
  if (spawns || llvm::any_of(getMatchedArchetypes(query),
                             [&](ArchetypeOp archetype) {
                               return isStructuralFor(query, archetype);
                             }))
    return std::string("it changes which entities archetypes hold, which "
                       "is applied in row order");
  return std::nullopt;
}

/// Open the event logs of a reactive query's triggers, at the insertion
/// point: note where every segment ends now, which is as far as the query
/// reads this time. Returns whether it has to visit every entity instead
/// (i1: its first run, or a log was overwritten since it last read it) and
/// how many entries wait for it in all (i64).
static std::pair<Value, Value> openLogs(IRRewriter &rewriter, QueryOp query,
                                        const WorldLayout &layout,
                                        WorldAccess &world, Value seen) {
  Location loc = query.getLoc();
  SmallVector<Trigger> triggers = getTriggers(query);
  auto index =
      query->getAttrOfType<IntegerAttr>(WorldLayout::kReactiveIndexAttr);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value pending;
  // Note where every segment ends now; scan if this is the first run or
  // a segment holds more entries since this query read it than it keeps.
  Value scan = arith::CmpIOp::create(
      rewriter, loc, arith::CmpIPredicate::eq, seen,
      arith::ConstantIntOp::create(rewriter, loc, 0, 64));
  pending = arith::ConstantIntOp::create(rewriter, loc, 0, 64);
  for (auto [k, trigger] : llvm::enumerate(triggers)) {
    const WorldLog &log = *layout.findLog(getStamp(trigger));
    Value positions =
        world.logPositions(log, layout.readPositions[index.getInt()][k]);
    auto segments = scf::ForOp::create(
        rewriter, loc, zero,
        arith::ConstantIndexOp::create(rewriter, loc, log.segments), one,
        ValueRange{scan, pending});
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(segments.getBody());
    Value segment = segments.getInductionVar();
    Value countAt = arith::MulIOp::create(
        rewriter, loc, segment,
        arith::ConstantIndexOp::create(rewriter, loc,
                                       WorldLog::kSegmentStride / 8));
    Value end = memref::LoadOp::create(rewriter, loc, world.logCounts(log),
                                       ValueRange{countAt});
    memref::StoreOp::create(rewriter, loc, end, world.logEnds(log),
                            ValueRange{segment});
    Value from = memref::LoadOp::create(rewriter, loc, positions,
                                        ValueRange{segment});
    // Lost: more entries than the segment holds, or an event that was
    // not appended since this query last read it (see appendToLog).
    Value lost = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::sgt,
        arith::SubIOp::create(rewriter, loc, end, from),
        arith::ConstantIntOp::create(rewriter, loc, log.segmentCapacity,
                                     64));
    lost = arith::OrIOp::create(
        rewriter, loc, lost,
        arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::slt, from,
            memref::LoadOp::create(
                rewriter, loc, world.logCounts(log),
                ValueRange{arith::AddIOp::create(
                    rewriter, loc, countAt,
                    arith::ConstantIndexOp::create(rewriter, loc, 2))})));
    Value more = arith::AddIOp::create(
        rewriter, loc, segments.getRegionIterArg(1),
        arith::SubIOp::create(rewriter, loc, end, from));
    scf::YieldOp::create(
        rewriter, loc,
        ValueRange{arith::OrIOp::create(rewriter, loc,
                                        segments.getRegionIterArg(0), lost),
                   more});
    rewriter.setInsertionPointAfter(segments);
    scan = segments.getResult(0);
    pending = segments.getResult(1);
  }
  return {scan, pending};
}

/// The query has read every segment of its triggers' logs to where it
/// ended when the query started; the slowest reader of each segment bounds
/// how far writers append.
static void closeLogs(IRRewriter &rewriter, QueryOp query,
                      const WorldLayout &layout, WorldAccess &world) {
  Location loc = query.getLoc();
  SmallVector<Trigger> triggers = getTriggers(query);
  auto index =
      query->getAttrOfType<IntegerAttr>(WorldLayout::kReactiveIndexAttr);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  for (auto [k, trigger] : llvm::enumerate(triggers)) {
    const WorldLog &log = *layout.findLog(getStamp(trigger));
    Value positions =
        world.logPositions(log, layout.readPositions[index.getInt()][k]);
    auto segments = scf::ForOp::create(
        rewriter, loc, zero,
        arith::ConstantIndexOp::create(rewriter, loc, log.segments), one);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPoint(segments.getBody()->getTerminator());
    Value segment = segments.getInductionVar();
    Value end = memref::LoadOp::create(rewriter, loc, world.logEnds(log),
                                       ValueRange{segment});
    memref::StoreOp::create(rewriter, loc, end, positions,
                            ValueRange{segment});
    Value slowest;
    for (uint64_t reader : log.readerOffsets) {
      Value position = memref::LoadOp::create(
          rewriter, loc, world.logPositions(log, reader),
          ValueRange{segment});
      slowest = slowest ? arith::MinSIOp::create(rewriter, loc, slowest,
                                                 position)
                              .getResult()
                        : position;
    }
    Value slowestAt = arith::AddIOp::create(
        rewriter, loc,
        arith::MulIOp::create(
            rewriter, loc, segment,
            arith::ConstantIndexOp::create(rewriter, loc,
                                           WorldLog::kSegmentStride / 8)),
        one);
    memref::StoreOp::create(rewriter, loc, slowest, world.logCounts(log),
                            ValueRange{slowestAt});
    // An event lost from here on is news to this reader again.
    memref::StoreOp::create(
        rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
        world.logCounts(log),
        ValueRange{arith::AddIOp::create(
                       rewriter, loc, slowestAt,
                       arith::ConstantIndexOp::create(rewriter, loc, 2))
                       .getResult()});
  }
}

//===----------------------------------------------------------------------===//
// Parts emitted more than once
//===----------------------------------------------------------------------===//

/// A query that follows its events has its body, and what marks the
/// entities to go to, in several places: where it goes through
/// everything, where it follows, where it visits the entities without a
/// parent. Each such part is emitted where it is used, in a region with
/// a name (`emitPart`); when the function is lowered, the parts with one
/// name become one function that the places call (`shareParts`).
static const char kPartAttr[] = "ent.part";

/// Emit what `emit` emits at the insertion point as the part `name`; as
/// it is, if `name` is empty. The insertion point is after it then.
static void emitPart(IRRewriter &rewriter, Location loc, const Twine &name,
                     function_ref<void()> emit) {
  std::string key = name.str();
  if (key.empty()) {
    OpBuilder::InsertionGuard guard(rewriter);
    emit();
    return;
  }
  auto part = scf::ExecuteRegionOp::create(rewriter, loc, TypeRange{});
  part->setAttr(kPartAttr, rewriter.getStringAttr(key));
  Block *block = part.getRegion().empty()
                     ? rewriter.createBlock(&part.getRegion())
                     : &part.getRegion().front();
  rewriter.setInsertionPointToStart(block);
  emit();
  rewriter.setInsertionPointToEnd(block);
  scf::YieldOp::create(rewriter, loc);
  rewriter.setInsertionPointAfter(part);
}

/// The same for a part that gives values (of `types`), which `emit`
/// returns, emitting from the insertion point on and leaving it at the
/// part's end.
static SmallVector<Value>
emitPartWith(IRRewriter &rewriter, Location loc, StringRef name,
             TypeRange types, function_ref<SmallVector<Value>()> emit) {
  auto part = scf::ExecuteRegionOp::create(rewriter, loc, types);
  part->setAttr(kPartAttr, rewriter.getStringAttr(name));
  Block *block = part.getRegion().empty()
                     ? rewriter.createBlock(&part.getRegion())
                     : &part.getRegion().front();
  rewriter.setInsertionPointToStart(block);
  SmallVector<Value> values = emit();
  scf::YieldOp::create(rewriter, loc, values);
  rewriter.setInsertionPointAfter(part);
  return SmallVector<Value>(part->getResults());
}

namespace {
/// Two parts gone through side by side: which value of the second stands
/// where which value of the first does, for those defined inside.
struct PartMatch {
  llvm::DenseMap<Value, Value> inner;
  llvm::DenseSet<Value> innerOfSecond;
};
} // namespace

static bool matchRegions(Region &a, Region &b, PartMatch &match);

/// Whether two ops are alike but for the values they take from outside
/// their parts and for their constants (which count as from outside:
/// where the parts have different ones, as for the columns of different
/// archetypes, the function is given them).
static bool matchOps(Operation &a, Operation &b, PartMatch &match) {
  if (a.hasTrait<OpTrait::ConstantLike>() &&
      b.hasTrait<OpTrait::ConstantLike>())
    return a.getName() == b.getName() &&
           a.getResult(0).getType() == b.getResult(0).getType();
  if (a.getName() != b.getName() ||
      a.getAttrDictionary() != b.getAttrDictionary() ||
      a.getPropertiesAsAttribute() != b.getPropertiesAsAttribute() ||
      a.getNumOperands() != b.getNumOperands() ||
      a.getNumResults() != b.getNumResults() ||
      a.getNumRegions() != b.getNumRegions())
    return false;
  for (auto [x, y] : llvm::zip(a.getOperands(), b.getOperands())) {
    if (x.getType() != y.getType())
      return false;
    auto known = match.inner.find(x);
    if (known != match.inner.end()) {
      if (known->second != y)
        return false;
    } else if (match.innerOfSecond.contains(y)) {
      return false;
    }
  }
  for (auto [x, y] : llvm::zip(a.getResults(), b.getResults())) {
    if (x.getType() != y.getType())
      return false;
    match.inner[x] = y;
    match.innerOfSecond.insert(y);
  }
  for (auto [x, y] : llvm::zip(a.getRegions(), b.getRegions()))
    if (!matchRegions(x, y, match))
      return false;
  return true;
}

static bool matchRegions(Region &a, Region &b, PartMatch &match) {
  if (std::distance(a.begin(), a.end()) != std::distance(b.begin(), b.end()))
    return false;
  for (auto [x, y] : llvm::zip(a, b)) {
    if (x.getNumArguments() != y.getNumArguments() ||
        std::distance(x.begin(), x.end()) != std::distance(y.begin(), y.end()))
      return false;
    for (auto [p, q] : llvm::zip(x.getArguments(), y.getArguments())) {
      if (p.getType() != q.getType())
        return false;
      match.inner[p] = q;
      match.innerOfSecond.insert(q);
    }
    for (auto [p, q] : llvm::zip(x, y))
      if (!matchOps(p, q, match))
        return false;
  }
  return true;
}

/// The uses, in a part, of what it takes from outside (its constants
/// too), in the order the ops come: the same places in parts that are
/// alike.
static void outerUses(Region &region, llvm::DenseSet<Value> &inner,
                      SmallVectorImpl<OpOperand *> &uses) {
  for (Block &block : region) {
    for (Value argument : block.getArguments())
      inner.insert(argument);
    for (Operation &op : block) {
      if (op.hasTrait<OpTrait::ConstantLike>())
        continue;
      for (OpOperand &operand : op.getOpOperands())
        if (!inner.contains(operand.get()))
          uses.push_back(&operand);
      for (Value result : op.getResults())
        inner.insert(result);
      for (Region &nested : op.getRegions())
        outerUses(nested, inner, uses);
    }
  }
}

/// Put what is in the part `part` in its place, as if it had no name.
static void dissolvePart(IRRewriter &rewriter, scf::ExecuteRegionOp part) {
  Block &block = part.getRegion().front();
  SmallVector<Value> given(block.getTerminator()->getOperands());
  rewriter.eraseOp(block.getTerminator());
  rewriter.inlineBlockBefore(&block, part);
  rewriter.replaceOp(part, given);
}

/// A value a function can make for itself rather than be given: a
/// constant.
static bool isMadeAnew(Value value) {
  Operation *def = value.getDefiningOp();
  return def && def->hasTrait<OpTrait::ConstantLike>();
}

/// The parts of `func` with one name become one function, called where
/// they were: for the values each takes from outside, which stand in the
/// same places in all of them (they are emitted by the same code), the
/// function has parameters. A part that is the only one of its name, or
/// that is not like the first of them, stays where it is.
static void shareParts(IRRewriter &rewriter, func::FuncOp func) {
  static unsigned serial = 0;
  // Innermost first: a part in a part is a call by the time the outer
  // ones are compared.
  SmallVector<scf::ExecuteRegionOp> parts;
  func.walk<WalkOrder::PostOrder>([&](scf::ExecuteRegionOp part) {
    if (part->hasAttr(kPartAttr))
      parts.push_back(part);
  });
  llvm::MapVector<Attribute, SmallVector<scf::ExecuteRegionOp>> byName;
  for (scf::ExecuteRegionOp part : parts)
    byName[part->getAttr(kPartAttr)].push_back(part);
  // Those of a name that are alike, together: each with the first that
  // it is like.
  SmallVector<SmallVector<scf::ExecuteRegionOp>> groups;
  for (auto &[name, named] : byName) {
    size_t from = groups.size();
    for (scf::ExecuteRegionOp part : named) {
      part->removeAttr(kPartAttr);
      bool placed = false;
      for (size_t k = from; k < groups.size() && !placed; ++k) {
        PartMatch match;
        if (matchRegions(groups[k].front().getRegion(), part.getRegion(),
                         match)) {
          groups[k].push_back(part);
          placed = true;
        }
      }
      if (!placed)
        groups.push_back({part});
    }
  }
  // Innermost first: by how many parts deep the parts in each are (the
  // same for all of a group, which are alike).
  llvm::DenseMap<Operation *, unsigned> height;
  for (scf::ExecuteRegionOp part : parts) {
    unsigned own = height.lookup(part) + 1;
    height[part] = own;
    for (Operation *around = part->getParentOp(); around;
         around = around->getParentOp())
      if (llvm::is_contained(parts, dyn_cast<scf::ExecuteRegionOp>(around))) {
        height[around] = std::max(height.lookup(around), own);
        break;
      }
  }
  llvm::stable_sort(groups, [&](const auto &a, const auto &b) {
    return height.lookup(a.front()) < height.lookup(b.front());
  });
  for (auto &group : groups) {
    scf::ExecuteRegionOp first = group.front();
    if (group.size() == 1) {
      dissolvePart(rewriter, first);
      continue;
    }
    // What each takes from outside, place by place. The places where all
    // of them take the same from the same are one parameter; a place
    // where each has the same constant needs none.
    SmallVector<SmallVector<OpOperand *>> uses(group.size());
    for (auto [part, places] : llvm::zip(group, uses)) {
      llvm::DenseSet<Value> inner;
      outerUses(part.getRegion(), inner, places);
    }
    size_t places = uses.front().size();
    auto sameConstant = [](Value a, Value b) {
      return isMadeAnew(a) && isMadeAnew(b) && a.getType() == b.getType() &&
             a.getDefiningOp()->getName() == b.getDefiningOp()->getName() &&
             a.getDefiningOp()->getAttrDictionary() ==
                 b.getDefiningOp()->getAttrDictionary() &&
             a.getDefiningOp()->getPropertiesAsAttribute() ==
                 b.getDefiningOp()->getPropertiesAsAttribute();
    };
    // (A slot: the places with the same values in every part.)
    std::map<std::vector<void *>, unsigned> slots;
    SmallVector<unsigned> slotOf(places);
    SmallVector<size_t> firstPlace;
    for (size_t place = 0; place < places; ++place) {
      std::vector<void *> values;
      for (auto &list : uses)
        values.push_back(list[place]->get().getAsOpaquePointer());
      unsigned next = slots.size();
      auto [entry, isNew] = slots.insert({values, next});
      slotOf[place] = entry->second;
      if (isNew)
        firstPlace.push_back(place);
    }
    // A slot is given to the function, or made in it: a constant that is
    // the same in all, or a view of the world's memory, which the
    // function makes from the memory and from where in it the view is
    // (given too, where the parts have different ones: the columns of
    // different archetypes).
    enum Kind { Given, Made, View, ViewAt };
    SmallVector<Kind> kinds(firstPlace.size(), Given);
    auto viewOf = [](Value value) {
      return dyn_cast_or_null<memref::ViewOp>(value.getDefiningOp());
    };
    Value memory;
    for (auto [slot, place] : llvm::enumerate(firstPlace)) {
      Value value = uses.front()[place]->get();
      bool constant = true, view = true, sameShift = true;
      memref::ViewOp firstView = viewOf(value);
      for (auto &list : uses) {
        Value there = list[place]->get();
        constant &= sameConstant(value, there);
        memref::ViewOp other = viewOf(there);
        view &= firstView && other && there.getType() == value.getType() &&
                other.getSource() == firstView.getSource() &&
                other.getSizes().empty() && isMadeAnew(other.getByteShift());
        if (view)
          sameShift &=
              sameConstant(firstView.getByteShift(), other.getByteShift());
      }
      if (constant) {
        kinds[slot] = Made;
      } else if (view) {
        kinds[slot] = sameShift ? View : ViewAt;
        memory = firstView.getSource();
      }
    }
    SmallVector<Type> types;
    if (memory)
      types.push_back(memory.getType());
    SmallVector<unsigned> parameter(firstPlace.size());
    for (auto [slot, place] : llvm::enumerate(firstPlace)) {
      if (kinds[slot] == Given || kinds[slot] == ViewAt) {
        parameter[slot] = types.size();
        types.push_back(kinds[slot] == Given
                            ? uses.front()[place]->get().getType()
                            : Type(rewriter.getIndexType()));
      }
    }
    Location loc = first.getLoc();
    // The arguments of each, before anything moves.
    SmallVector<SmallVector<Value>> arguments(group.size());
    for (auto [list, values] : llvm::zip(uses, arguments)) {
      if (memory)
        values.push_back(memory);
      for (auto [slot, place] : llvm::enumerate(firstPlace)) {
        Value value = list[place]->get();
        if (kinds[slot] == Given)
          values.push_back(value);
        else if (kinds[slot] == ViewAt)
          values.push_back(viewOf(value).getByteShift());
      }
    }
    func::FuncOp shared;
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(func);
      shared = func::FuncOp::create(
          rewriter, loc, ("ent_part_" + Twine(serial++)).str(),
          rewriter.getFunctionType(types, first->getResultTypes()));
      shared.setPrivate();
      Block *entry = shared.addEntryBlock();
      rewriter.setInsertionPointToStart(entry);
      auto anew = [&](Value value) -> Value {
        return rewriter.clone(*value.getDefiningOp())
            ->getResult(cast<OpResult>(value).getResultNumber());
      };
      SmallVector<Value> inside(firstPlace.size());
      for (auto [slot, place] : llvm::enumerate(firstPlace)) {
        Value value = uses.front()[place]->get();
        switch (kinds[slot]) {
        case Given:
          inside[slot] = entry->getArgument(parameter[slot]);
          break;
        case Made:
          inside[slot] = anew(value);
          break;
        case View:
        case ViewAt:
          inside[slot] = memref::ViewOp::create(
              rewriter, loc, cast<MemRefType>(value.getType()),
              entry->getArgument(0),
              kinds[slot] == View
                  ? anew(viewOf(value).getByteShift())
                  : Value(entry->getArgument(parameter[slot])),
              ValueRange{});
          break;
        }
      }
      // The first one's ops move in, taking what they took from outside
      // from the function, which gives what the part gave.
      for (auto [place, use] : llvm::enumerate(uses.front()))
        use->set(inside[slotOf[place]]);
      Block &block = first.getRegion().front();
      SmallVector<Value> given(block.getTerminator()->getOperands());
      rewriter.eraseOp(block.getTerminator());
      SmallVector<Operation *> ops;
      for (Operation &op : block)
        ops.push_back(&op);
      for (Operation *op : ops)
        op->moveBefore(entry, entry->end());
      rewriter.setInsertionPointToEnd(entry);
      func::ReturnOp::create(rewriter, loc, given);
    }
    for (auto [part, values] : llvm::zip(group, arguments)) {
      rewriter.setInsertionPoint(part);
      // (A constant of the part's own goes with the part: made anew.)
      for (Value &value : values)
        if (isMadeAnew(value))
          value = rewriter.clone(*value.getDefiningOp())
                      ->getResult(cast<OpResult>(value).getResultNumber());
      auto call = func::CallOp::create(rewriter, part.getLoc(), shared, values);
      rewriter.replaceOp(part, call.getResults());
    }
  }

}


/// Marks: a bit per position in i64 words (`marks`), for the entities a
/// query that follows events has yet to look at. Set the bit of
/// `position` (an index), at the insertion point.
static void markBit(IRRewriter &rewriter, Location loc, Value marks,
                    Value position) {
  Value sixtyFour = arith::ConstantIndexOp::create(rewriter, loc, 64);
  Value word = arith::DivUIOp::create(rewriter, loc, position, sixtyFour);
  Value bit = arith::ShLIOp::create(
      rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 1, 64),
      arith::IndexCastOp::create(
          rewriter, loc, rewriter.getI64Type(),
          arith::RemUIOp::create(rewriter, loc, position, sixtyFour)));
  Value old = memref::LoadOp::create(rewriter, loc, marks, ValueRange{word});
  memref::StoreOp::create(rewriter, loc,
                          arith::OrIOp::create(rewriter, loc, old, bit), marks,
                          ValueRange{word});
}

/// Go through the marks of the first `count` positions from the lowest,
/// at the insertion point: each is taken away and `visit` emits what
/// happens at its position, which may set marks of later positions. The
/// word is read again for every mark, since those may be in it too.
/// Returns the loop.
static scf::ForOp sweepMarks(IRRewriter &rewriter, Location loc, Value marks,
                             Value count, function_ref<void(Value)> visit) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value sixtyFour = arith::ConstantIndexOp::create(rewriter, loc, 64);
  Value words = arith::DivUIOp::create(
      rewriter, loc,
      arith::AddIOp::create(rewriter, loc, count,
                            arith::ConstantIndexOp::create(rewriter, loc, 63)),
      sixtyFour);
  auto sweep = scf::ForOp::create(rewriter, loc, zero, words, one);
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(sweep.getBody()->getTerminator());
  Value word = sweep.getInductionVar();
  Type i64 = rewriter.getI64Type();
  scf::WhileOp::create(
      rewriter, loc, TypeRange{}, ValueRange{},
      [&](OpBuilder &, Location, ValueRange) {
        scf::ConditionOp::create(
            rewriter, loc,
            arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::ne,
                memref::LoadOp::create(rewriter, loc, marks, ValueRange{word}),
                arith::ConstantIntOp::create(rewriter, loc, 0, 64)),
            ValueRange{});
      },
      [&](OpBuilder &, Location, ValueRange) {
        Value bits =
            memref::LoadOp::create(rewriter, loc, marks, ValueRange{word});
        memref::StoreOp::create(
            rewriter, loc,
            arith::AndIOp::create(
                rewriter, loc, bits,
                arith::SubIOp::create(
                    rewriter, loc, bits,
                    arith::ConstantIntOp::create(rewriter, loc, 1, 64))),
            marks, ValueRange{word});
        Value lowest = LLVM::CountTrailingZerosOp::create(
            rewriter, loc, i64, bits, /*is_zero_poison=*/true);
        {
          OpBuilder::InsertionGuard inner(rewriter);
          visit(arith::AddIOp::create(
              rewriter, loc,
              arith::MulIOp::create(rewriter, loc, word, sixtyFour),
              arith::IndexCastOp::create(rewriter, loc,
                                         rewriter.getIndexType(), lowest)));
        }
        scf::YieldOp::create(rewriter, loc, ValueRange{});
      });
  return sweep;
}

/// As sweepMarks, from the highest mark of the first `count` positions
/// to the lowest: `visit` may set marks of earlier positions.
static scf::ForOp sweepMarksDown(IRRewriter &rewriter, Location loc,
                                 Value marks, Value count,
                                 function_ref<void(Value)> visit) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value sixtyFour = arith::ConstantIndexOp::create(rewriter, loc, 64);
  Value words = arith::DivUIOp::create(
      rewriter, loc,
      arith::AddIOp::create(rewriter, loc, count,
                            arith::ConstantIndexOp::create(rewriter, loc, 63)),
      sixtyFour);
  auto sweep = scf::ForOp::create(rewriter, loc, zero, words, one);
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(sweep.getBody()->getTerminator());
  Value word = arith::SubIOp::create(
      rewriter, loc, arith::SubIOp::create(rewriter, loc, words, one),
      sweep.getInductionVar());
  Type i64 = rewriter.getI64Type();
  scf::WhileOp::create(
      rewriter, loc, TypeRange{}, ValueRange{},
      [&](OpBuilder &, Location, ValueRange) {
        scf::ConditionOp::create(
            rewriter, loc,
            arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::ne,
                memref::LoadOp::create(rewriter, loc, marks, ValueRange{word}),
                arith::ConstantIntOp::create(rewriter, loc, 0, 64)),
            ValueRange{});
      },
      [&](OpBuilder &, Location, ValueRange) {
        Value bits =
            memref::LoadOp::create(rewriter, loc, marks, ValueRange{word});
        Value highest = arith::SubIOp::create(
            rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 63, 64),
            LLVM::CountLeadingZerosOp::create(rewriter, loc, i64, bits,
                                              /*is_zero_poison=*/true));
        memref::StoreOp::create(
            rewriter, loc,
            arith::XOrIOp::create(
                rewriter, loc, bits,
                arith::ShLIOp::create(
                    rewriter, loc,
                    arith::ConstantIntOp::create(rewriter, loc, 1, 64),
                    highest)),
            marks, ValueRange{word});
        {
          OpBuilder::InsertionGuard inner(rewriter);
          visit(arith::AddIOp::create(
              rewriter, loc,
              arith::MulIOp::create(rewriter, loc, word, sixtyFour),
              arith::IndexCastOp::create(rewriter, loc,
                                         rewriter.getIndexType(), highest)));
        }
        scf::YieldOp::create(rewriter, loc, ValueRange{});
      });
  return sweep;
}

/// As sweepMarks, for the marks of the positions from `from` to `to`
/// only; the others stay as they are.
static scf::ForOp sweepMarkRange(IRRewriter &rewriter, Location loc,
                                 Value marks, Value from, Value to,
                                 function_ref<void(Value)> visit) {
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value sixtyFour = arith::ConstantIndexOp::create(rewriter, loc, 64);
  Type i64 = rewriter.getI64Type();
  auto asBits = [&](Value index) -> Value {
    return arith::IndexCastOp::create(rewriter, loc, i64, index);
  };
  Value first = arith::DivUIOp::create(rewriter, loc, from, sixtyFour);
  Value past = arith::SelectOp::create(
      rewriter, loc,
      arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult, from,
                            to),
      arith::DivUIOp::create(
          rewriter, loc,
          arith::AddIOp::create(
              rewriter, loc, to,
              arith::ConstantIndexOp::create(rewriter, loc, 63)),
          sixtyFour),
      first);
  auto sweep = scf::ForOp::create(rewriter, loc, first, past, one);
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(sweep.getBody()->getTerminator());
  Value word = sweep.getInductionVar();
  Value base = arith::MulIOp::create(rewriter, loc, word, sixtyFour);
  // The word's bits that are positions of the range.
  Value low = arith::SubIOp::create(
      rewriter, loc, arith::MaxUIOp::create(rewriter, loc, from, base), base);
  Value high = arith::SubIOp::create(
      rewriter, loc,
      arith::MinUIOp::create(
          rewriter, loc, to,
          arith::AddIOp::create(rewriter, loc, base, sixtyFour)),
      base);
  Value ones = arith::ConstantIntOp::create(rewriter, loc, -1, 64);
  Value mask = arith::AndIOp::create(
      rewriter, loc, arith::ShLIOp::create(rewriter, loc, ones, asBits(low)),
      arith::ShRUIOp::create(
          rewriter, loc, ones,
          arith::SubIOp::create(
              rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 64, 64),
              asBits(high))));
  auto inRange = [&]() -> Value {
    return arith::AndIOp::create(
        rewriter, loc,
        memref::LoadOp::create(rewriter, loc, marks, ValueRange{word}), mask);
  };
  scf::WhileOp::create(
      rewriter, loc, TypeRange{}, ValueRange{},
      [&](OpBuilder &, Location, ValueRange) {
        scf::ConditionOp::create(
            rewriter, loc,
            arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::ne, inRange(),
                arith::ConstantIntOp::create(rewriter, loc, 0, 64)),
            ValueRange{});
      },
      [&](OpBuilder &, Location, ValueRange) {
        Value bits = inRange();
        Value lowest = LLVM::CountTrailingZerosOp::create(
            rewriter, loc, i64, bits, /*is_zero_poison=*/true);
        Value all =
            memref::LoadOp::create(rewriter, loc, marks, ValueRange{word});
        memref::StoreOp::create(
            rewriter, loc,
            arith::XOrIOp::create(
                rewriter, loc, all,
                arith::ShLIOp::create(
                    rewriter, loc,
                    arith::ConstantIntOp::create(rewriter, loc, 1, 64),
                    lowest)),
            marks, ValueRange{word});
        {
          OpBuilder::InsertionGuard inner(rewriter);
          visit(arith::AddIOp::create(
              rewriter, loc, base,
              arith::IndexCastOp::create(rewriter, loc,
                                         rewriter.getIndexType(), lowest)));
        }
        scf::YieldOp::create(rewriter, loc, ValueRange{});
      });
  return sweep;
}

/// For every entry of the event logs of a reactive query's triggers that
/// the query has not read (see openLogs), at the insertion point: `event`
/// emits what happens for the trigger and the entity's id, which may be
/// of an entity that is no more.
static void
forEachEvent(IRRewriter &rewriter, QueryOp query, const WorldLayout &layout,
             WorldAccess &world,
             function_ref<void(const Trigger &, Value)> event) {
  Location loc = query.getLoc();
  SmallVector<Trigger> triggers = getTriggers(query);
  auto index =
      query->getAttrOfType<IntegerAttr>(WorldLayout::kReactiveIndexAttr);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  for (auto [k, trigger] : llvm::enumerate(triggers)) {
    const WorldLog &log = *layout.findLog(getStamp(trigger));
    Value positions =
        world.logPositions(log, layout.readPositions[index.getInt()][k]);
    auto segments = scf::ForOp::create(
        rewriter, loc, zero,
        arith::ConstantIndexOp::create(rewriter, loc, log.segments), one);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPoint(segments.getBody()->getTerminator());
    Value segment = segments.getInductionVar();
    Value from = memref::LoadOp::create(rewriter, loc, positions,
                                        ValueRange{segment});
    Value to = memref::LoadOp::create(rewriter, loc, world.logEnds(log),
                                      ValueRange{segment});
    Value first = arith::MulIOp::create(
        rewriter, loc, segment,
        arith::ConstantIndexOp::create(rewriter, loc, log.segmentCapacity));
    auto entries = scf::ForOp::create(rewriter, loc, world.toIndex(loc, from),
                                      world.toIndex(loc, to), one);
    rewriter.setInsertionPoint(entries.getBody()->getTerminator());
    Value slot = arith::AddIOp::create(
        rewriter, loc, first,
        arith::AndIOp::create(
            rewriter, loc, entries.getInductionVar(),
            arith::ConstantIndexOp::create(rewriter, loc,
                                           log.segmentCapacity - 1)));
    event(trigger, memref::LoadOp::create(rewriter, loc, world.logIds(log),
                                          ValueRange{slot}));
  }
}

/// Open the logs of a reactive query that follows events down its tree
/// (see openLogs), and return whether it goes through all `count`
/// positions of the tree's order instead: where events were lost, and
/// where they are many. Following costs several times a visit per entity
/// it comes to, so with events for more than a sixteenth of the positions
/// (those of the query's own last run are among them) it does not pay.
static Value openLogsToFollow(IRRewriter &rewriter, QueryOp query,
                              const WorldLayout &layout, WorldAccess &world,
                              Value seen, Value count) {
  Location loc = query.getLoc();
  auto [lost, pending] = openLogs(rewriter, query, layout, world, seen);
  return arith::OrIOp::create(
      rewriter, loc, lost,
      arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::sgt,
          arith::MulIOp::create(
              rewriter, loc, pending,
              arith::ConstantIntOp::create(rewriter, loc, 16, 64)),
          arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                     count)));
}

/// Walk the event logs of a reactive query's triggers, segment by segment,
/// from where it last read each (its positions) to where it ended when the
/// query started (the log's ends), at the insertion point, and run the
/// query's body for each entity with an event: an entry counts if it is
/// the entity's latest in this log (its stamp still holds the entry's
/// tick), the entity matches the query, and no earlier trigger of the query
/// fired for it too, so each entity runs once. With `parallel`, the
/// segments are walked in parallel: an entity's latest entry is in one
/// segment only, so no two iterations run the body for the same entity.
static void walkLogs(IRRewriter &rewriter, QueryOp query,
                     const WorldLayout &layout, WorldAccess &world, Value tick,
                     Value seen, bool parallel) {
  Location loc = query.getLoc();
  SmallVector<Trigger> triggers = getTriggers(query);
  auto index =
      query->getAttrOfType<IntegerAttr>(WorldLayout::kReactiveIndexAttr);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  for (unsigned k = 0; k < triggers.size(); ++k) {
    Stamp stamp = getStamp(triggers[k]);
    const WorldLog &log = *layout.findLog(stamp);
    Value positions =
        world.logPositions(log, layout.readPositions[index.getInt()][k]);
    Value count = arith::ConstantIndexOp::create(rewriter, loc, log.segments);
    Operation *segments;
    Value segment;
    OpBuilder::InsertionGuard guard(rewriter);
    if (parallel) {
      auto loop = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                          ValueRange{count}, ValueRange{one});
      rewriter.setInsertionPoint(loop.getBody()->getTerminator());
      segments = loop;
      segment = loop.getInductionVars().front();
    } else {
      auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
      rewriter.setInsertionPoint(loop.getBody()->getTerminator());
      segments = loop;
      segment = loop.getInductionVar();
    }
    Value from = memref::LoadOp::create(rewriter, loc, positions,
                                        ValueRange{segment});
    Value to = memref::LoadOp::create(rewriter, loc, world.logEnds(log),
                                      ValueRange{segment});
    Value first = arith::MulIOp::create(
        rewriter, loc, segment,
        arith::ConstantIndexOp::create(rewriter, loc, log.segmentCapacity));
    auto entries = scf::ForOp::create(rewriter, loc, world.toIndex(loc, from),
                                      world.toIndex(loc, to), one);
    rewriter.setInsertionPoint(entries.getBody()->getTerminator());
    Value slot = arith::AddIOp::create(
        rewriter, loc, first,
        arith::AndIOp::create(
            rewriter, loc, entries.getInductionVar(),
            arith::ConstantIndexOp::create(rewriter, loc,
                                           log.segmentCapacity - 1)));
    Value id = memref::LoadOp::create(rewriter, loc, world.logIds(log),
                                      ValueRange{slot});
    Value when = memref::LoadOp::create(rewriter, loc, world.logTicks(log),
                                        ValueRange{slot});
    auto candidate = [&](const WorldArchetype &archetype) {
      return matches(archetype, query) && archetype.findStamp(stamp);
    };
    emitLocate(
        rewriter, loc, layout, world, id, candidate, FlatSymbolRefAttr(),
        TypeRange{},
        [&](const WorldArchetype &archetype, Value row,
            Value) -> SmallVector<Value> {
          OpBuilder::InsertionGuard inner(rewriter);
          Value current = memref::LoadOp::create(
              rewriter, loc,
              world.stamps(archetype, *archetype.findStamp(stamp)),
              ValueRange{row});
          Value runs = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::eq, current, when);
          for (const Trigger &earlier : ArrayRef(triggers).take_front(k)) {
            const WorldColumn *column =
                archetype.findStamp(getStamp(earlier));
            if (!column)
              continue;
            Value stamped = memref::LoadOp::create(
                rewriter, loc, world.stamps(archetype, *column),
                ValueRange{row});
            Value fired = arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::sgt, stamped, seen);
            Value notFired = arith::XOrIOp::create(
                rewriter, loc, fired,
                arith::ConstantIntOp::create(rewriter, loc, 1, 1));
            runs = arith::AndIOp::create(rewriter, loc, runs, notFired);
          }
          auto branch = scf::IfOp::create(rewriter, loc, runs);
          rewriter.setInsertionPointToStart(branch.thenBlock());
          bool sends = false;
          query.getBody().walk([&](Operation *op) {
            sends |= isa<ApplyOp, AccumulateOp, ConnectOp>(op);
          });
          emitPart(rewriter, loc, sends || parallel ? "" : "body", [&] {
            emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                          layout, row, world.count(loc, archetype), tick,
                          Value(), parallel);
          });
          return {};
        },
        []() -> SmallVector<Value> { return {}; });
    rewriter.setInsertionPointAfter(segments);
    hoistResourceReads(rewriter, segments, world);
  }
}

/// Fold the values `accumulate` sent from the first `count` rows of
/// `archetype` into its resource field, at the insertion point, one row
/// after another (rows that sent nothing are skipped), so the result does
/// not depend on how the query's loop ran.
static void combineAccumulated(IRRewriter &rewriter, AccumulateOp accumulate,
                               const WorldArchetype &archetype,
                               WorldAccess &world, Value count) {
  Location loc = accumulate.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  auto [ids, values] = world.applyBuffer(accumulate, archetype);
  Value field = world.resourceField(accumulate.getResourceAttr().getAttr(),
                                    accumulate.getFieldAttr());
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value start = memref::LoadOp::create(rewriter, loc, field, ValueRange{zero});
  auto loop = scf::ForOp::create(rewriter, loc, zero, count, one,
                                 ValueRange{start});
  rewriter.setInsertionPointToStart(loop.getBody());
  Value row = loop.getInductionVar();
  Value sum = loop.getRegionIterArg(0);
  Value sent = arith::CmpIOp::create(
      rewriter, loc, arith::CmpIPredicate::ne,
      memref::LoadOp::create(rewriter, loc, ids, ValueRange{row}),
      world.noEntity(loc));
  Value value = memref::LoadOp::create(rewriter, loc, values, ValueRange{row});
  Value next = arith::SelectOp::create(
      rewriter, loc, sent,
      combine(rewriter, loc, accumulate.getRule(), sum, value), sum);
  scf::YieldOp::create(rewriter, loc, ValueRange{next});
  rewriter.setInsertionPointAfter(loop);
  memref::StoreOp::create(rewriter, loc, loop.getResult(0), field,
                          ValueRange{zero});
}

/// What a query's end does to the archetypes it changed the entities of,
/// at the insertion point: their pending despawns and moves are applied,
/// what depends on their rows is told (a sorted tree, and the locations a
/// tree keeps), and the relations in `changedRelations`, with those that
/// follow from that, are looked over.
static void commitStructure(IRRewriter &rewriter, Location loc,
                            const WorldLayout &layout, WorldAccess &world,
                            ArrayRef<const WorldArchetype *> changed,
                            llvm::SetVector<Attribute> &changedRelations,
                            Value tick) {
  for (const WorldArchetype *archetype : changed) {
    // A sorted archetype that loses entities is sorted again.
    if (archetype->isSorted()) {
      const WorldRelation &tree = layout.getRelation(archetype->sortedBy);
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value pending = memref::LoadOp::create(
          rewriter, loc, world.pendingCount(*archetype), ValueRange{zero});
      auto ifAny = scf::IfOp::create(
          rewriter, loc,
          arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::ne, pending,
              arith::ConstantIntOp::create(rewriter, loc, 0, 64)));
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(ifAny.thenBlock());
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
          world.edgesClean(tree), ValueRange{zero});
      changedRelations.insert(archetype->sortedBy);
    }
    // Rows of this archetype move: the locations that the trees keeping a
    // slot for its entities have noted are stale, and read again where
    // the relations are looked over below.
    for (const WorldRelation &tree : layout.relations) {
      if (!tree.hasLocations() || !canHoldEnd(*archetype, tree))
        continue;
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value pending = memref::LoadOp::create(
          rewriter, loc, world.pendingCount(*archetype), ValueRange{zero});
      auto ifAny = scf::IfOp::create(
          rewriter, loc,
          arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::ne, pending,
              arith::ConstantIntOp::create(rewriter, loc, 0, 64)));
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(ifAny.thenBlock());
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 1, 64),
          world.treeStale(tree), ValueRange{zero});
      changedRelations.insert(RelationOp(tree.op).getSymNameAttr());
    }
    applyPending(rewriter, loc, layout, *archetype, world, tick);
  }
  // Then the changed relations are sorted, which also drops edges to the
  // entities just despawned.
  for (Attribute relation : changedRelations)
    callSort(rewriter, loc, layout.getRelation(cast<StringAttr>(relation)),
             world.getArena());
}

/// The trees whose children are ordered by a field that `query` may write
/// join `changedRelations`: they are looked over when the query ends. A
/// set marks its tree unclean where it runs (see lowerAccesses), and a
/// value applied or combined into the field where it lands
/// (combineAtRow); an add of the component does here, at the insertion
/// point, whether it runs for any entity or not.
static void noteOrderWrites(IRRewriter &rewriter, QueryOp query,
                            const WorldLayout &layout, WorldAccess &world,
                            llvm::SetVector<Attribute> &changedRelations) {
  Location loc = query.getLoc();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(query);
  for (const WorldRelation &relation : layout.relations) {
    if (!relation.isOrdered())
      continue;
    bool sets = false, others = false;
    query.getBody().walk([&](Operation *op) {
      if (auto set = dyn_cast<SetOp>(op))
        sets |= cast<RefType>(set.getRef().getType())
                        .getComponent()
                        .getAttr() == relation.orderComponent &&
                set.getFieldAttr() == relation.orderField;
      else if (auto add = dyn_cast<AddOp>(op))
        others |= add.getComponentAttr().getAttr() == relation.orderComponent;
      else if (auto apply = dyn_cast<ApplyOp>(op))
        sets |=
            apply.getComponentAttr().getAttr() == relation.orderComponent &&
            apply.getFieldAttr() == relation.orderField;
      else if (auto combine = dyn_cast<CombineOp>(op))
        sets |= combine.getRef().getType().getComponent().getAttr() ==
                    relation.orderComponent &&
                combine.getFieldAttr() == relation.orderField;
    });
    if (!sets && !others)
      continue;
    changedRelations.insert(RelationOp(relation.op).getSymNameAttr());
    if (others)
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 64),
          world.edgesClean(relation),
          ValueRange{arith::ConstantIndexOp::create(rewriter, loc, 0)
                         .getResult()});
  }
}

/// Replace a cascading query by loops that visit parents before their
/// children: first the entities without a parent in the tree, archetype
/// after archetype, then those with one in the order the relation's sort
/// left, each found by its id. Entities of one depth read
/// only what shallower ones wrote (through refs up the tree), so one pass
/// in this order is the query run once per depth. A query with a ref up
/// the tree it cascades along visits no entity without a parent. With
/// `leaves first` the order is the other way: the list from its end, then
/// the entities without a parent. What the query combines into ancestors
/// it combines as it visits, which is the order of the depths' ends.
/// True if a cascading query's body only reads and writes the entity it
/// visits and its ancestors' components (through refs up the tree), so
/// the entities of one depth can be visited in any order, or at once.
static bool isDepthLocal(QueryOp query) {
  WalkResult result = query.getBody().walk([](Operation *op) {
    if (isa<GetOp, SetOp, ReadOp, EntityOp, HasOp, LookupOp, CombineOp,
            YieldOp>(op) ||
        !hasOwnEffects(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !result.wasInterrupted();
}

static void lowerCascade(IRRewriter &rewriter, QueryOp query,
                         const WorldLayout &layout, WorldAccess &world,
                         const LoopOptions &options) {
  Location loc = query.getLoc();
  FlatSymbolRefAttr cascade = query.getCascade();
  const WorldRelation &relation = layout.getRelation(cascade.getAttr());
  rewriter.setInsertionPoint(query);
  // A reactive query takes the tick it last started at and advances the
  // counter, as any does. It goes through its tree as ever, and its body
  // applies where a trigger fired: no event log is walked, since the
  // order is the tree's.
  SmallVector<Trigger> triggers = getTriggers(query);
  if (!triggers.empty() && options.explain)
    query.emitRemark(layout.cascadeFollowsEvents(query)
                         ? "follows its events along the tree"
                         : "goes through its whole tree on each run");
  Value seen;
  if (!triggers.empty()) {
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value last = world.lastTick(query);
    seen = memref::LoadOp::create(rewriter, loc, last, ValueRange{zero});
    Value next = world.currentTick(loc);
    memref::StoreOp::create(rewriter, loc, next, last, ValueRange{zero});
    memref::StoreOp::create(rewriter, loc, next, world.tickCounter(),
                            ValueRange{zero});
  }
  Value tick = world.hasStamps() ? world.currentTick(loc) : Value();
  bool matched = false;
  // Despawns take effect when the whole query has run. Its rows are not
  // visited in their order, which applying them goes by: so a despawn
  // marks its row in the row's own place of the archetype's pending list
  // (cleared here), and the marked rows are made the list, in order, when
  // the query has run.
  // The same for an entity that an add or a remove moves to another
  // archetype. A query visits the entities there are when it starts: the
  // archetypes are counted now, since a body may spawn into one whose
  // loop comes later.
  llvm::DenseMap<const WorldArchetype *, Value> startCounts;
  for (const WorldArchetype &archetype : layout.archetypes)
    if (matches(archetype, query))
      startCounts[&archetype] = world.count(loc, archetype);
  SmallVector<std::pair<const WorldArchetype *, Value>> despawning;
  for (const WorldArchetype &archetype : layout.archetypes)
    if (matches(archetype, query) && archetype.hasPending() &&
        isStructuralFor(query, archetype.op))
      despawning.push_back({&archetype, startCounts.lookup(&archetype)});
  bool marks = !despawning.empty();
  // Edges the query connects are added when it has run, from a place per
  // row that says "none" unless the row's entity connected; and the
  // relations whose edges it changes, or whose sorted archetype it spawns
  // into, are looked over then.
  SmallVector<ConnectOp> connects;
  query.getBody().walk([&](ConnectOp connect) { connects.push_back(connect); });
  llvm::SetVector<Attribute> changedRelations;
  noteOrderWrites(rewriter, query, layout, world, changedRelations);
  for (ConnectOp connect : connects)
    changedRelations.insert(connect.getRelationAttr().getAttr());
  query.getBody().walk([&](DisconnectOp disconnect) {
    changedRelations.insert(
        disconnect->getParentOfType<EdgesOp>().getRelationAttr().getAttr());
  });
  query.getBody().walk([&](SpawnOp spawn) {
    for (const WorldArchetype &archetype : layout.archetypes)
      if (archetype.isSorted() &&
          ArchetypeOp(archetype.op).getSymNameAttr() ==
              spawn.getArchetypeAttr().getAttr())
        changedRelations.insert(archetype.sortedBy);
  });
  for (ConnectOp connect : connects)
    for (const WorldArchetype &archetype : layout.archetypes) {
      Value rows = startCounts.lookup(&archetype);
      if (!rows)
        continue;
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
      auto clear = scf::ForOp::create(rewriter, loc, zero, rows, one);
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(clear.getBody()->getTerminator());
      memref::StoreOp::create(
          rewriter, loc, world.noEntity(loc),
          world.connectBuffer(connect, archetype).sources,
          ValueRange{clear.getInductionVar()});
    }
  {
    for (auto &[archetypePointer, rows] : despawning) {
      const WorldArchetype &archetype = *archetypePointer;
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
      auto clear = scf::ForOp::create(rewriter, loc, zero, rows, one);
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(clear.getBody()->getTerminator());
      memref::StoreOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 0, 32),
          world.pendingList(archetype),
          ValueRange{clear.getInductionVar()});
    }
  }
  // (An entity without a parent has no ancestor and no sibling: a ref
  // up or before the tree leaves it out, unless it is optional.)
  bool visitsRoots =
      llvm::none_of(query.getBody().getArgumentTypes(), [&](Type type) {
        auto ref = cast<RefType>(type);
        return ref.getVia() == cascade && !ref.getIsOptional();
      });
  LoopOptions sequential = options;
  sequential.parallelEntities = false;
  bool leavesFirst = query.isLeavesFirst();
  bool readsSiblings =
      llvm::any_of(query.getBody().getArgumentTypes(), [](Type type) {
        auto ref = cast<RefType>(type);
        return ref.getIsBefore() || ref.getIsAfter();
      });
  // The archetype stored in the tree's order, if there is one: its rows
  // are the entities without a parent, then the others, parents first.
  const WorldArchetype *sorted =
      relation.sortedArchetype() >= 0
          ? &layout.archetypes[relation.sortedArchetype()]
          : nullptr;
  // Or the several a sorted tree lives in, each with its rows by depth.
  SmallVector<const WorldArchetype *> levelled;
  if (relation.sortedArchetypes.size() > 1)
    for (unsigned index : relation.sortedArchetypes)
      levelled.push_back(&layout.archetypes[index]);
  auto rootCount = [&](const WorldArchetype &archetype) {
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    return world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, world.rootCount(archetype),
                                    ValueRange{zero}));
  };
  // The body for the entity at `row` of `archetype`, at the insertion
  // point. A query that follows its events along a list has it in several
  // places, which share it (see emitPart).
  // (And one that goes in an order worked out comes to each entity by
  // its id, whatever archetype it is in.)
  bool shares = (layout.cascadeFollowsEvents(query) &&
                 !layout.cascadeFollows(query, /*links=*/false)) ||
                relation.walksInOrder(query.getTraversal());
  std::string partName = shares ? "query" : "";
  auto bodyAt = [&](const WorldArchetype &archetype, Value row,
                    KnownParent known) {
    emitPart(rewriter, loc,
             shares ? partName + ".body" : Twine(),
             [&] {
               emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                             layout, row, world.count(loc, archetype), tick,
                             seen, /*parallel=*/false, /*directApplies=*/true,
                             known, marks);
             });
  };
  // (Before `rootsAt`: the query, or where a query that follows events
  // goes through everything instead.)
  Operation *rootsAt = query;
  auto emitRoots = [&] {
  for (const WorldArchetype &archetype : layout.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    if (!visitsRoots)
      continue;
    rewriter.setInsertionPoint(rootsAt);
    if (&archetype == sorted || llvm::is_contained(levelled, &archetype)) {
      Operation *loops = emitEntityLoops(
          rewriter, loc, archetype, world, sequential, /*entityLocal=*/false,
          [&](Value entity, Value rows, bool) {
            emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                          layout, entity, rows, tick, seen,
                          /*parallel=*/false, /*directApplies=*/true, KnownParent(),
                        marks);
          },
          rootCount(archetype));
      hoistResourceReads(rewriter, loops, world);
      continue;
    }
    Operation *loops = emitEntityLoops(
        rewriter, loc, archetype, world, sequential, /*entityLocal=*/false,
        [&](Value entity, Value rows, bool) {
          Value hasParent =
              emitParent(rewriter, loc, layout, world, relation,
                         world.entityId(loc, archetype, entity))
                  .first;
          auto ifRoot = scf::IfOp::create(
              rewriter, loc,
              arith::XOrIOp::create(
                  rewriter, loc, hasParent,
                  arith::ConstantIntOp::create(rewriter, loc, 1, 1)));
          rewriter.setInsertionPointToStart(ifRoot.thenBlock());
          bodyAt(archetype, entity, KnownParent());
        },
        startCounts.lookup(&archetype));
    hoistResourceReads(rewriter, loops, world);
  }
  };
  // An order asked for exactly, where it is not the one the tree is
  // stored in: worked out from the tree's list, which has every entity
  // with a parent after that parent, into a list of its own.
  StringRef traversal = query.getTraversal();
  // (Or one a query that follows its events is to go by, over a tree
  // without links: depth first.)
  bool byWalk = layout.cascadeFollowsByWalk(query);
  bool inOrder = relation.walksInOrder(traversal) || byWalk;
  bool depthFirst = traversal == "dfs" || (byWalk && traversal.empty());
  // (A query that follows its events to those without a parent visits
  // them where it goes through everything, further down.)
  if (!leavesFirst && !(inOrder && depthFirst) &&
      !layout.cascadeFollowsToRoots(query))
    emitRoots();
  else
    matched = llvm::any_of(layout.archetypes,
                           [&](const WorldArchetype &archetype) {
                             return matches(archetype, query);
                           });
  // A sorted tree gone through by its rows (before `rowsAt`: the query,
  // or where a query that follows its events another way goes through
  // everything instead).
  Operation *rowsAt = query;
  auto goThroughRows = [&] {
    if (sorted) {
      // Every entity with a parent is in the sorted archetype: its rows
      // after those without one, up or down, each with its parent's row.
      if (matches(*sorted, query)) {
        rewriter.setInsertionPoint(rowsAt);
        Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
        Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
        Value rows = world.count(loc, *sorted);
        Value roots = rootCount(*sorted);
        auto parentOf = [&](Value row) {
          return world.toIndex(
              loc, memref::LoadOp::create(rewriter, loc,
                                          world.parentRows(*sorted),
                                          ValueRange{row}));
        };
        auto body = [&](Value row, Value parentRow, bool parallel) {
          KnownParent parent{&relation, Value(), sorted, parentRow};
          emitQueryBody(rewriter, query, IRMapping(), *sorted, world, layout,
                        row, rows, tick, seen, parallel,
                        /*directApplies=*/true, parent, marks);
        };
        // The rows from `from` to `to`, up, or down for children first.
        auto inOrder = [&](Value from, Value to,
                           bool parallel = false) -> Operation * {
          auto loop = scf::ForOp::create(
              rewriter, loc, zero, arith::SubIOp::create(rewriter, loc, to, from),
              one);
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPoint(loop.getBody()->getTerminator());
          Value row = leavesFirst
                          ? arith::SubIOp::create(
                                rewriter, loc,
                                arith::SubIOp::create(rewriter, loc, to, one),
                                loop.getInductionVar())
                                .getResult()
                          : arith::AddIOp::create(rewriter, loc, from,
                                                  loop.getInductionVar())
                                .getResult();
          body(row, parentOf(row), parallel);
          return loop;
        };
        // The entities of one depth do not depend on each other, so a depth
        // can be visited in parallel: where the body only reads and writes
        // the entity and its ancestors and the archetype can be large
        // enough. Going depth by depth costs a little for every depth, which
        // a deep tree has more of than it gains: so only where a depth holds
        // 256 entities on average (or parallel-min-level, if that is less),
        // and then each depth of at least parallel-min-level entities in
        // parallel. Else the rows after one another, as they are.
        bool combines = false;
        query.getBody().walk([&](CombineOp) { combines = true; });
        // (Siblings are visited one after another where a ref is to the
        // one before.)
        bool mayRunInParallel =
            options.parallelEntities && isDepthLocal(query) && !marks &&
            triggers.empty() && !readsSiblings &&
            connects.empty() && !(combines && world.hasStamps()) &&
            sorted->capacity >= options.parallelMinEntities;
        if (!mayRunInParallel && layout.cascadeFollows(query, /*links=*/false)) {
          // Reactive, going where the events lead (see
          // WorldLayout::cascadeFollowsEvents): the rows are in the tree's
          // order, a row's children next to each other after it, and a mark
          // per row says which the query has yet to look at. An entity with
          // an event of its own gets one, and so do the children of an
          // entity with an event that a trigger up the tree means; then the
          // marked rows from the first, each passing marks on to its
          // children if the body changed it. (An event marks more rows
          // than the body runs for, which the body's own test of the ticks
          // sorts out.) On the first run, and where events were lost or are
          // many, every row as ever.
          Value marked = world.treeMarks(relation);
          auto forRows = [&](Value from, Value to, function_ref<void(Value)> at) {
            auto loop = scf::ForOp::create(rewriter, loc, from, to, one);
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPoint(loop.getBody()->getTerminator());
            at(loop.getInductionVar());
          };
          auto markChildren = [&](Value row) {
            auto edge = [&](bool begin) {
              return world.toIndex(
                  loc, memref::LoadOp::create(rewriter, loc,
                                              world.childRows(*sorted, begin),
                                              ValueRange{row}));
            };
            forRows(edge(true), edge(false),
                    [&](Value child) { markBit(rewriter, loc, marked, child); });
          };
          Value scan =
              openLogsToFollow(rewriter, query, layout, world, seen, rows);
          auto scanOrFollow = scf::IfOp::create(rewriter, loc, scan,
                                                /*withElseRegion=*/true);
          rewriter.setInsertionPointToStart(scanOrFollow.thenBlock());
          hoistResourceReads(rewriter, inOrder(roots, rows), world);
          rewriter.setInsertionPointToStart(scanOrFollow.elseBlock());
          forEachEvent(
              rewriter, query, layout, world,
              [&](const Trigger &trigger, Value id) {
                emitLocate(
                    rewriter, loc, layout, world, id,
                    [&](const WorldArchetype &archetype) {
                      return &archetype == sorted;
                    },
                    FlatSymbolRefAttr(), TypeRange{},
                    [&](const WorldArchetype &, Value row,
                        Value) -> SmallVector<Value> {
                      OpBuilder::InsertionGuard inner(rewriter);
                      if (trigger.via)
                        markChildren(row);
                      else
                        markBit(rewriter, loc, marked, row);
                      return {};
                    },
                    []() -> SmallVector<Value> { return {}; });
              });
          scf::ForOp sweep = sweepMarks(rewriter, loc, marked, rows, [&](Value row) {
            // (An entity without a parent can have had an event; it is not
            // visited.)
            auto ifBelow = scf::IfOp::create(
                rewriter, loc,
                arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::uge,
                                      row, roots));
            rewriter.setInsertionPointToStart(ifBelow.thenBlock());
            auto ran = scf::IfOp::create(
                rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 1, 1));
            {
              OpBuilder::InsertionGuard inner(rewriter);
              rewriter.setInsertionPointToStart(ran.thenBlock());
              body(row, parentOf(row), /*parallel=*/false);
            }
            // What the body changed here that a trigger up the tree means
            // has this query's tick, which no other event has.
            Value changedHere;
            for (const Trigger &trigger : triggers) {
              const WorldColumn *column =
                  trigger.via ? sorted->findStamp(getStamp(trigger)) : nullptr;
              if (!column)
                continue;
              Value now = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq,
                  memref::LoadOp::create(rewriter, loc,
                                         world.stamps(*sorted, *column),
                                         ValueRange{row}),
                  tick);
              changedHere = changedHere
                                ? arith::OrIOp::create(rewriter, loc,
                                                       changedHere, now)
                                      .getResult()
                                : now;
            }
            if (changedHere) {
              auto ifChanged = scf::IfOp::create(rewriter, loc, changedHere);
              rewriter.setInsertionPointToStart(ifChanged.thenBlock());
              markChildren(row);
            }
          });
          hoistResourceReads(rewriter, sweep, world);
          rewriter.setInsertionPoint(rowsAt);
          closeLogs(rewriter, query, layout, world);
        } else if (!mayRunInParallel) {
          hoistResourceReads(rewriter, inOrder(roots, rows), world);
        } else {
          Value depths = world.toIndex(
              loc, memref::LoadOp::create(rewriter, loc,
                                          world.treeDepth(relation),
                                          ValueRange{zero}));
          Value level = arith::ConstantIndexOp::create(
              rewriter, loc, options.parallelMinLevel);
          Value wide = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::sge,
              arith::SubIOp::create(rewriter, loc, rows, roots),
              arith::MulIOp::create(
                  rewriter, loc, depths,
                  arith::ConstantIndexOp::create(
                      rewriter, loc,
                      std::min<int64_t>(256, options.parallelMinLevel))));
          auto choice = scf::IfOp::create(rewriter, loc, wide,
                                          /*withElseRegion=*/true);
          rewriter.setInsertionPointToStart(choice.elseBlock());
          inOrder(roots, rows);
          rewriter.setInsertionPointToStart(choice.thenBlock());
          Value starts = world.levelStarts(*sorted);
          auto startOf = [&](Value depth) {
            return world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc, starts,
                                            ValueRange{depth}));
          };
          auto levels = scf::ForOp::create(rewriter, loc, zero, depths, one);
          rewriter.setInsertionPoint(levels.getBody()->getTerminator());
          Value depth =
              leavesFirst
                  ? arith::SubIOp::create(rewriter, loc, depths,
                                          levels.getInductionVar())
                        .getResult()
                  : arith::AddIOp::create(rewriter, loc,
                                          levels.getInductionVar(), one)
                        .getResult();
          Value from = startOf(depth);
          Value to = startOf(arith::AddIOp::create(rewriter, loc, depth, one));
          Value many = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::sge,
              arith::SubIOp::create(rewriter, loc, to, from), level);
          auto perDepth = scf::IfOp::create(rewriter, loc, many,
                                            /*withElseRegion=*/true);
          rewriter.setInsertionPointToStart(perDepth.elseBlock());
          inOrder(from, to);
          rewriter.setInsertionPointToStart(perDepth.thenBlock());
          if (!combines) {
            auto each = scf::ParallelOp::create(rewriter, loc, ValueRange{from},
                                                ValueRange{to}, ValueRange{one});
            rewriter.setInsertionPoint(each.getBody()->getTerminator());
            Value row = each.getInductionVars().front();
            body(row, parentOf(row), /*parallel=*/true);
          } else {
            // What entities send to their parent is added up per parent, so
            // the children of one parent stay with one thread: the depth's
            // rows are cut into pieces at the rows where a parent's children
            // begin, and the pieces run in parallel, each its rows after one
            // another as they are when all are, so the sums are the same to
            // the bit.
            Value pieces = arith::ConstantIndexOp::create(rewriter, loc, 64);
            Value number = arith::SubIOp::create(rewriter, loc, to, from);
            Value last = arith::SubIOp::create(rewriter, loc, to, one);
            auto each = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                                ValueRange{pieces},
                                                ValueRange{one});
            rewriter.setInsertionPoint(each.getBody()->getTerminator());
            Value piece = each.getInductionVars().front();
            // Where piece `index` begins: the first row of the children that
            // the row at its share of the depth is one of.
            auto beginOf = [&](Value index) -> Value {
              Value share = arith::AddIOp::create(
                  rewriter, loc, from,
                  arith::DivUIOp::create(
                      rewriter, loc,
                      arith::MulIOp::create(rewriter, loc, index, number),
                      pieces));
              Value row = arith::MinUIOp::create(rewriter, loc, share, last);
              Value begin = world.toIndex(
                  loc, memref::LoadOp::create(
                           rewriter, loc,
                           world.childRows(*sorted, /*begin=*/true),
                           ValueRange{parentOf(row)}));
              Value isFirst = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq, index, zero);
              Value isPast = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq, index, pieces);
              return arith::SelectOp::create(
                  rewriter, loc, isFirst, from,
                  arith::SelectOp::create(rewriter, loc, isPast, to, begin));
            };
            Value begin = beginOf(piece);
            Value end =
                beginOf(arith::AddIOp::create(rewriter, loc, piece, one));
            inOrder(begin, end, /*parallel=*/true);
          }
          hoistResourceReads(rewriter, choice, world);
        }
      }
      if (leavesFirst)
        emitRoots();
    } else {
      // Depth by depth, from the first under the roots down or from the
      // last up; in each, the rows every archetype has of it, each with its
      // parent's location. Children first takes the archetypes and the rows
      // the other way round too, so that it is parents first backwards.
      rewriter.setInsertionPoint(rowsAt);
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
      Value depths = world.toIndex(
          loc, memref::LoadOp::create(rewriter, loc, world.treeDepth(relation),
                                      ValueRange{zero}));
      bool sends = false;
      query.getBody().walk([&](CombineOp) { sends = true; });
      int64_t held = 0;
      for (const WorldArchetype *archetype : levelled)
        held += archetype->capacity;
      bool levelsInParallel =
          options.parallelEntities && isDepthLocal(query) && !marks &&
          triggers.empty() && connects.empty() && !relation.isOrdered() &&
          !(sends && (world.hasStamps() ||
                      levelled.front()->childRangeOffsets.empty())) &&
          held >= options.parallelMinEntities;
      auto walkDepths = [&] {
        auto levels = scf::ForOp::create(rewriter, loc, zero, depths, one);
        {
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPoint(levels.getBody()->getTerminator());
          Value depth =
              leavesFirst
                  ? arith::SubIOp::create(rewriter, loc, depths,
                                          levels.getInductionVar())
                        .getResult()
                  : arith::AddIOp::create(rewriter, loc,
                                          levels.getInductionVar(), one)
                        .getResult();
          Value next = arith::AddIOp::create(rewriter, loc, depth, one);
          SmallVector<const WorldArchetype *> inOrder(levelled);
          if (leavesFirst)
            std::reverse(inOrder.begin(), inOrder.end());
          for (const WorldArchetype *archetype : inOrder) {
            if (!matches(*archetype, query))
              continue;
            rewriter.setInsertionPoint(levels.getBody()->getTerminator());
            Value starts = world.levelStarts(*archetype);
            Value from = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc, starts,
                                            ValueRange{depth}));
            Value to = world.toIndex(
                loc,
                memref::LoadOp::create(rewriter, loc, starts, ValueRange{next}));
            auto visit = [&](Value row, bool parallel) {
              KnownParent parent{&relation};
              parent.location = memref::LoadOp::create(
                  rewriter, loc, world.parentLocations(*archetype),
                  ValueRange{row});
              emitQueryBody(rewriter, query, IRMapping(), *archetype, world,
                            layout, row, world.count(loc, *archetype), tick, seen,
                            parallel, /*directApplies=*/true, parent, marks);
            };
            auto rowsInOrder = [&](Value from, Value to, bool parallel) {
              auto rows = scf::ForOp::create(
                  rewriter, loc, zero,
                  arith::SubIOp::create(rewriter, loc, to, from), one);
              OpBuilder::InsertionGuard inner(rewriter);
              rewriter.setInsertionPoint(rows.getBody()->getTerminator());
              visit(leavesFirst
                        ? arith::SubIOp::create(
                              rewriter, loc,
                              arith::SubIOp::create(rewriter, loc, to, one),
                              rows.getInductionVar())
                              .getResult()
                        : arith::AddIOp::create(rewriter, loc, from,
                                                rows.getInductionVar())
                              .getResult(),
                    parallel);
            };
            auto oneAfterAnother = [&] {
              rowsInOrder(from, to, /*parallel=*/false);
            };
            // The rows an archetype has of one depth do not depend on each
            // other, nor on those the other archetypes have of it: in
            // parallel where there are parallel-min-level of them, the body
            // only reads and writes its entity and reads or adds into its
            // ancestors, and
            // the archetype can be large enough.
            if (!levelsInParallel || archetype->capacity < options.parallelMinLevel) {
              oneAfterAnother();
              continue;
            }
            Value many = arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::sge,
                arith::SubIOp::create(rewriter, loc, to, from),
                arith::ConstantIndexOp::create(rewriter, loc,
                                               options.parallelMinLevel));
            auto perDepth = scf::IfOp::create(rewriter, loc, many,
                                              /*withElseRegion=*/true);
            rewriter.setInsertionPointToStart(perDepth.elseBlock());
            oneAfterAnother();
            rewriter.setInsertionPointToStart(perDepth.thenBlock());
            if (!sends) {
              auto each = scf::ParallelOp::create(rewriter, loc,
                                                  ValueRange{from},
                                                  ValueRange{to},
                                                  ValueRange{one});
              rewriter.setInsertionPoint(each.getBody()->getTerminator());
              visit(each.getInductionVars().front(), /*parallel=*/true);
              continue;
            }
            // What entities send to their parent is added up per parent, in
            // the order they are visited. The archetypes take their turns
            // as ever, and one archetype's rows of the depth are cut into
            // pieces at rows where a parent's children begin (they are next
            // to each other), which run in parallel, each its rows in order:
            // a parent gets what it gets from one thread at a time and in
            // the order it always does, so the sums are the same to the bit.
            unsigned holder =
                llvm::find(levelled, archetype) - levelled.begin();
            Value pieces = arith::ConstantIndexOp::create(rewriter, loc, 64);
            Value number = arith::SubIOp::create(rewriter, loc, to, from);
            Value last = arith::SubIOp::create(rewriter, loc, to, one);
            auto each = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                                ValueRange{pieces},
                                                ValueRange{one});
            rewriter.setInsertionPoint(each.getBody()->getTerminator());
            Value piece = each.getInductionVars().front();
            auto beginOf = [&](Value index) -> Value {
              Value share = arith::AddIOp::create(
                  rewriter, loc, from,
                  arith::DivUIOp::create(
                      rewriter, loc,
                      arith::MulIOp::create(rewriter, loc, index, number),
                      pieces));
              Value row = arith::MinUIOp::create(rewriter, loc, share, last);
              auto [where, parentRow] = world.unpackLocation(
                  loc, memref::LoadOp::create(rewriter, loc,
                                              world.parentLocations(*archetype),
                                              ValueRange{row}));
              // The first of the row's siblings here, which its parent has,
              // wherever that is.
              Value begin = row;
              for (const WorldArchetype *home : levelled) {
                auto ifThere = scf::IfOp::create(
                    rewriter, loc, TypeRange{rewriter.getIndexType()},
                    arith::CmpIOp::create(
                        rewriter, loc, arith::CmpIPredicate::eq, where,
                        arith::ConstantIntOp::create(
                            rewriter, loc, home->index,
                            layout.entities.locationBits)),
                    /*withElseRegion=*/true);
                OpBuilder::InsertionGuard inner(rewriter);
                rewriter.setInsertionPointToStart(ifThere.thenBlock());
                scf::YieldOp::create(
                    rewriter, loc,
                    ValueRange{world.toIndex(
                        loc,
                        memref::LoadOp::create(
                            rewriter, loc,
                            world.childRange(*home, holder, /*begin=*/true),
                            ValueRange{parentRow}))});
                rewriter.setInsertionPointToStart(ifThere.elseBlock());
                scf::YieldOp::create(rewriter, loc, ValueRange{begin});
                begin = ifThere.getResult(0);
              }
              Value isFirst = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq, index, zero);
              Value isPast = arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::eq, index, pieces);
              return arith::SelectOp::create(
                  rewriter, loc, isFirst, from,
                  arith::SelectOp::create(rewriter, loc, isPast, to, begin));
            };
            Value begin = beginOf(piece);
            Value end =
                beginOf(arith::AddIOp::create(rewriter, loc, piece, one));
            rowsInOrder(begin, end, /*parallel=*/true);
          }
        }
        hoistResourceReads(rewriter, levels, world);
      };
      // A deep tree has few rows a depth, and going depth by depth costs
      // more than the rows do. It is gone through by the tree's list
      // instead, which has every entity after its parent, with where its
      // row is and where its parent's: one loop, and a branch per entity on
      // its archetype. (Within a depth that is the list's order, not the
      // archetypes' one after another.)
      Value rowsInAll = zero;
      for (const WorldArchetype *archetype : levelled)
        rowsInAll = arith::AddIOp::create(rewriter, loc, rowsInAll,
                                          world.count(loc, *archetype));
      auto walkList = [&] {
        Value count = world.toIndex(
            loc, memref::LoadOp::create(rewriter, loc,
                                        world.treeOrderCount(relation),
                                        ValueRange{zero}));
        auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
        {
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPoint(loop.getBody()->getTerminator());
          Value at = loop.getInductionVar();
          if (leavesFirst)
            at = arith::SubIOp::create(
                rewriter, loc, arith::SubIOp::create(rewriter, loc, count, one),
                at);
          Value id = memref::LoadOp::create(
              rewriter, loc, world.treeOrder(relation), ValueRange{at});
          KnownParent parent{&relation};
          parent.location = memref::LoadOp::create(
              rewriter, loc, world.rowOrder(relation, /*parent=*/true),
              ValueRange{at});
          Value location = memref::LoadOp::create(
              rewriter, loc, world.rowOrder(relation, /*parent=*/false),
              ValueRange{at});
          emitLocate(
              rewriter, loc, layout, world, id,
              [&](const WorldArchetype &archetype) {
                return llvm::is_contained(levelled, &archetype);
              },
              FlatSymbolRefAttr(), TypeRange{},
              [&](const WorldArchetype &archetype, Value row,
                  Value) -> SmallVector<Value> {
                if (!matches(archetype, query))
                  return {};
                OpBuilder::InsertionGuard inner(rewriter);
                emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                              layout, row, world.count(loc, archetype), tick,
                              seen, /*parallel=*/false, /*directApplies=*/true,
                              parent, marks);
                return {};
              },
              []() -> SmallVector<Value> { return {}; }, LocateBounds(),
              /*trusted=*/true, location);
        }
        hoistResourceReads(rewriter, loop, world);
      };
      auto walkLevels = [&] {
        // (A reactive query stays with the depths: by the list it took
        // twice as long in a deep tree, 2.9 ms against 1.4 at a million
        // nodes, where the query without triggers takes half. Why is not
        // known.)
        // A tree whose children are in an order goes by its list whatever
        // it is like: depth by depth, an archetype's children of a parent
        // would come before another's that are before them.
        if (relation.isOrdered()) {
          walkList();
          return;
        }
        if (!triggers.empty()) {
          walkDepths();
          return;
        }
        Value deep = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::ugt,
            arith::MulIOp::create(
                rewriter, loc, depths,
                arith::ConstantIndexOp::create(rewriter, loc, 64)),
            rowsInAll);
        auto byListOrDepth = scf::IfOp::create(rewriter, loc, deep,
                                               /*withElseRegion=*/true);
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(byListOrDepth.thenBlock());
        walkList();
        rewriter.setInsertionPointToStart(byListOrDepth.elseBlock());
        walkDepths();
      };
      if (!layout.cascadeFollows(query, /*links=*/false)) {
        walkLevels();
      } else {
        // Reactive, going where the events lead (see
        // WorldLayout::cascadeFollowsEvents), as over a tree sorted in one
        // archetype: a mark per row of every archetype, set for the rows
        // with an event and for the children of those with an event that a
        // trigger up the tree means, which each row has the rows of in
        // every archetype. Then depth by depth the marked rows every
        // archetype has of it, each marking its children if the body
        // changed it. Every row instead on the first run, where events
        // were lost or are many, and where the tree is so deep that going
        // through its depths costs more than its rows (fewer than 64 a
        // depth).
        auto forRows = [&](Value from, Value to, function_ref<void(Value)> at) {
          auto loop = scf::ForOp::create(rewriter, loc, from, to, one);
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPoint(loop.getBody()->getTerminator());
          at(loop.getInductionVar());
        };
        auto markChildren = [&](const WorldArchetype &home, Value row) {
          for (auto [index, archetype] : llvm::enumerate(levelled)) {
            auto edge = [&](bool begin) {
              return world.toIndex(
                  loc, memref::LoadOp::create(
                           rewriter, loc, world.childRange(home, index, begin),
                           ValueRange{row}));
            };
            Value marked = world.rowMarks(*archetype);
            forRows(edge(true), edge(false),
                    [&](Value child) { markBit(rewriter, loc, marked, child); });
          }
        };
        Value all = zero;
        for (const WorldArchetype *archetype : levelled)
          all = arith::AddIOp::create(rewriter, loc, all,
                                      world.count(loc, *archetype));
        Value scan = arith::OrIOp::create(
            rewriter, loc,
            openLogsToFollow(rewriter, query, layout, world, seen, all),
            arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::ugt,
                arith::MulIOp::create(
                    rewriter, loc, depths,
                    arith::ConstantIndexOp::create(rewriter, loc, 64)),
                all));
        auto scanOrFollow = scf::IfOp::create(rewriter, loc, scan,
                                              /*withElseRegion=*/true);
        rewriter.setInsertionPointToStart(scanOrFollow.thenBlock());
        walkLevels();
        rewriter.setInsertionPointToStart(scanOrFollow.elseBlock());
        forEachEvent(
            rewriter, query, layout, world,
            [&](const Trigger &trigger, Value id) {
              emitLocate(
                  rewriter, loc, layout, world, id,
                  [&](const WorldArchetype &archetype) {
                    return llvm::is_contained(levelled, &archetype);
                  },
                  FlatSymbolRefAttr(), TypeRange{},
                  [&](const WorldArchetype &archetype, Value row,
                      Value) -> SmallVector<Value> {
                    OpBuilder::InsertionGuard inner(rewriter);
                    if (trigger.via) {
                      markChildren(archetype, row);
                      return {};
                    }
                    // (An entity without a parent is not visited, and its
                    // mark would stay.)
                    auto ifBelow = scf::IfOp::create(
                        rewriter, loc,
                        arith::CmpIOp::create(rewriter, loc,
                                              arith::CmpIPredicate::uge, row,
                                              rootCount(archetype)));
                    rewriter.setInsertionPointToStart(ifBelow.thenBlock());
                    markBit(rewriter, loc, world.rowMarks(archetype), row);
                    return {};
                  },
                  []() -> SmallVector<Value> { return {}; });
            });
        auto levels = scf::ForOp::create(rewriter, loc, zero, depths, one);
        {
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPoint(levels.getBody()->getTerminator());
          Value depth =
              arith::AddIOp::create(rewriter, loc, levels.getInductionVar(), one);
          Value next = arith::AddIOp::create(rewriter, loc, depth, one);
          for (const WorldArchetype *archetype : levelled) {
            Value starts = world.levelStarts(*archetype);
            Value from = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc, starts,
                                            ValueRange{depth}));
            Value to = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc, starts,
                                            ValueRange{next}));
            sweepMarkRange(
                rewriter, loc, world.rowMarks(*archetype), from, to,
                [&](Value row) {
                  // (A row of an archetype the query does not match has
                  // its mark taken, and that is all.)
                  if (!matches(*archetype, query))
                    return;
                  auto ran = scf::IfOp::create(
                      rewriter, loc,
                      arith::ConstantIntOp::create(rewriter, loc, 1, 1));
                  {
                    OpBuilder::InsertionGuard inner(rewriter);
                    rewriter.setInsertionPointToStart(ran.thenBlock());
                    KnownParent parent{&relation};
                    parent.location = memref::LoadOp::create(
                        rewriter, loc, world.parentLocations(*archetype),
                        ValueRange{row});
                    emitQueryBody(rewriter, query, IRMapping(), *archetype,
                                  world, layout, row,
                                  world.count(loc, *archetype), tick, seen,
                                  /*parallel=*/false, /*directApplies=*/true,
                                  parent, marks);
                  }
                  Value changedHere;
                  for (const Trigger &trigger : triggers) {
                    const WorldColumn *column =
                        trigger.via ? archetype->findStamp(getStamp(trigger))
                                    : nullptr;
                    if (!column)
                      continue;
                    Value now = arith::CmpIOp::create(
                        rewriter, loc, arith::CmpIPredicate::eq,
                        memref::LoadOp::create(
                            rewriter, loc, world.stamps(*archetype, *column),
                            ValueRange{row}),
                        tick);
                    changedHere = changedHere
                                      ? arith::OrIOp::create(rewriter, loc,
                                                             changedHere, now)
                                            .getResult()
                                      : now;
                  }
                  if (changedHere) {
                    auto ifChanged =
                        scf::IfOp::create(rewriter, loc, changedHere);
                    rewriter.setInsertionPointToStart(ifChanged.thenBlock());
                    markChildren(*archetype, row);
                  }
                });
          }
        }
        hoistResourceReads(rewriter, levels, world);
        rewriter.setInsertionPoint(rowsAt);
        closeLogs(rewriter, query, layout, world);
      }
      if (leavesFirst)
        emitRoots();
    }
  };
  // (The order's making, for the queries that go by it: where the order
  // is, where its entities' parents are, how many are below each, where
  // it starts and how many it has.)
  struct Walk {
    Value out, outParents, sizes, from, count;
  };
  auto buildWalk = [&](LinkedTree &tree) -> Walk {
    Value zero = tree.zero, one = tree.one;
    Type index = rewriter.getIndexType();
    Value keys =
        arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys + 1);
    Value list = world.treeOrder(relation);
    Value listParents = world.treeOrderParents(relation);
    Value listed = world.toIndex(loc, tree.listed());
    Value out = world.walkOrder(relation, /*parents=*/false);
    Value outParents = world.walkOrder(relation, /*parents=*/true);
    Value sizes = world.walkNumbers(relation, /*cursors=*/false);
    Value cursors = world.walkNumbers(relation, /*cursors=*/true);
    Value none = world.noEntity(loc);
    auto add = [&](Value a, Value b) -> Value {
      return arith::AddIOp::create(rewriter, loc, a, b);
    };
    auto asIndex = [&](Value number) -> Value {
      return arith::IndexCastOp::create(rewriter, loc, index, number);
    };
    // (An element of a linked tree's list may be empty.)
    auto forListed = [&](bool backwards,
                         function_ref<void(Value id, Value parent)> each) {
      tree.forEach(zero, listed, [&](Value k) {
        Value at = backwards
                       ? arith::SubIOp::create(
                             rewriter, loc,
                             arith::SubIOp::create(rewriter, loc, listed, one),
                             k)
                             .getResult()
                       : k;
        Value id = tree.load(list, at);
        tree.branch(tree.negate(tree.same(id, none)),
                    [&] { each(id, tree.load(listParents, at)); });
      });
    };
    // The order is kept until the tree changes (which takes the note of
    // it away, see forgetWalk): worked out only if it is not the one
    // that is there.
    Value state = world.walkState(relation);
    Value kind = tree.i64(!depthFirst ? 3 : leavesFirst ? 2 : 1);
    Value from = !depthFirst
                     ? arith::ConstantIndexOp::create(rewriter, loc,
                                                      2 * relation.capacity)
                           .getResult()
                     : zero;
    auto ifNotThere = scf::IfOp::create(
        rewriter, loc, tree.negate(tree.same(tree.load(state, zero), kind)));
    rewriter.setInsertionPointToStart(ifNotThere.thenBlock());
    Value count;
    // (Breadth first is made from the order with parents first.)
    bool after = leavesFirst && depthFirst;
    {
      // How many are below each entity, itself counted (one without a
      // parent is not in the list and not counted): from the list's end,
      // where an entity's children have been before it is come to.
      tree.forEach(zero, keys, [&](Value key) {
        tree.store(tree.i64(0), sizes, key);
        tree.store(tree.i64(-1), cursors, key);
      });
      // (Where each is in the order, for the queries that follow events
      // in it: nowhere, until it is given its place.)
      tree.forEach(zero,
                   arith::ConstantIndexOp::create(rewriter, loc,
                                                  layout.entityKeys),
                   [&](Value key) {
                     tree.store(tree.i64(-1), world.walkPlaces(relation), key);
                   });
      forListed(/*backwards=*/true, [&](Value id, Value parent) {
        Value key = world.entityKey(loc, id);
        Value size = add(tree.load(sizes, key), tree.i64(1));
        tree.store(size, sizes, key);
        Value parentKey = world.entityKey(loc, parent);
        tree.store(add(tree.load(sizes, parentKey), size), sizes, parentKey);
      });
      // Then every entity gets its place: all below one are next to each
      // other, after it (parents first) or before it, and an entity's
      // cursor is where the next of its children goes. One without a
      // parent is given room, and its place, when its first child comes.
      Value total = world.walkNumbers(relation, /*cursors=*/true);
      Value totalAt =
          arith::ConstantIndexOp::create(rewriter, loc, layout.entityKeys);
      tree.store(tree.i64(0), total, totalAt);
      auto place = [&](Value id, Value parent, Value at) {
        tree.store(id, out, asIndex(at));
        tree.store(parent, outParents, asIndex(at));
        tree.store(at, world.walkPlaces(relation), world.entityKey(loc, id));
      };
      forListed(/*backwards=*/false, [&](Value id, Value parent) {
        Value key = world.entityKey(loc, id);
        Value parentKey = world.entityKey(loc, parent);
        tree.branch(tree.same(tree.load(cursors, parentKey), tree.i64(-1)),
                    [&] {
          Value base = tree.load(total, totalAt);
          Value below = tree.load(sizes, parentKey);
          tree.store(add(base, add(below, tree.i64(1))), total, totalAt);
          if (after) {
            tree.store(base, cursors, parentKey);
            place(parent, none, add(base, below));
          } else {
            tree.store(add(base, tree.i64(1)), cursors, parentKey);
            place(parent, none, base);
          }
        });
        Value start = tree.load(cursors, parentKey);
        Value size = tree.load(sizes, key);
        tree.store(add(start, size), cursors, parentKey);
        if (after) {
          tree.store(start, cursors, key);
          place(id, parent, add(start, add(size, tree.i64(-1))));
        } else {
          tree.store(add(start, tree.i64(1)), cursors, key);
          place(id, parent, start);
        }
      });
      count = asIndex(tree.load(total, totalAt));
    }
    // Breadth first: the order above, parents first, taken depth by
    // depth. An entity's depth is its parent's and one (an entity without
    // a parent has none: 0, and is not in this order); how many there are
    // of each says where each depth starts, and the entities go there in
    // the order they have, so that the children of one are next to each
    // other and in their order.
    if (!depthFirst) {
      Value all = count;
      tree.forEach(zero, keys, [&](Value key) {
        tree.store(tree.i64(0), sizes, key);
        tree.store(tree.i64(0), cursors, key);
      });
      forListed(/*backwards=*/false, [&](Value id, Value parent) {
        Value depth = add(tree.load(sizes, world.entityKey(loc, parent)),
                          tree.i64(1));
        tree.store(depth, sizes, world.entityKey(loc, id));
        Value at = asIndex(depth);
        tree.store(add(tree.load(cursors, at), tree.i64(1)), cursors, at);
      });
      auto starts = scf::ForOp::create(rewriter, loc, zero, keys, one,
                                       ValueRange{tree.i64(0)});
      {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(starts.getBody());
        Value depth = starts.getInductionVar();
        Value start = starts.getRegionIterArg(0);
        Value number = tree.load(cursors, depth);
        tree.store(start, cursors, depth);
        scf::YieldOp::create(rewriter, loc, ValueRange{add(start, number)});
      }
      tree.forEach(zero, all, [&](Value k) {
        Value id = tree.load(out, k);
        Value parent = tree.load(outParents, k);
        tree.branch(tree.same(parent, none), [&] {
          tree.store(tree.i64(-1), world.walkPlaces(relation),
                     world.entityKey(loc, id));
        });
        tree.branch(tree.negate(tree.same(parent, none)), [&] {
          Value depth = asIndex(tree.load(sizes, world.entityKey(loc, id)));
          Value at = tree.load(cursors, depth);
          tree.store(add(at, tree.i64(1)), cursors, depth);
          Value to = arith::AddIOp::create(rewriter, loc, from, asIndex(at));
          tree.store(id, out, to);
          tree.store(parent, outParents, to);
          tree.store(arith::IndexCastOp::create(rewriter, loc,
                                                rewriter.getI64Type(), to),
                     world.walkPlaces(relation), world.entityKey(loc, id));
        });
      });
      count = asIndex(starts.getResult(0));
    }
    tree.store(arith::IndexCastOp::create(rewriter, loc,
                                          rewriter.getI64Type(), count),
               state, one);
    tree.store(kind, state, zero);
    rewriter.setInsertionPointAfter(ifNotThere);
    count = asIndex(tree.load(state, one));
    return {out, outParents, sizes, from, count};
  };
  // Depth first, an entity without a parent is in the order if it has
  // children, with them; one without is visited here, before the
  // others or (children first) after them.
  auto visitAlone = [&](LinkedTree &tree, Value sizes) {
    if (!depthFirst || !visitsRoots)
      return;
    for (const WorldArchetype &archetype : layout.archetypes) {
      if (!matches(archetype, query))
        continue;
      rewriter.setInsertionPoint(rootsAt);
      Operation *loops = emitEntityLoops(
          rewriter, loc, archetype, world, sequential,
          /*entityLocal=*/false,
          [&](Value entity, Value rows, bool) {
            Value id = world.entityId(loc, archetype, entity);
            Value hasParent =
                emitParent(rewriter, loc, layout, world, relation, id).first;
            Value alone = tree.both(
                tree.negate(hasParent),
                tree.same(tree.load(sizes, world.entityKey(loc, id)),
                          tree.i64(0)));
            auto ifAlone = scf::IfOp::create(rewriter, loc, alone);
            rewriter.setInsertionPointToStart(ifAlone.thenBlock());
            bodyAt(archetype, entity, KnownParent());
          },
          startCounts.lookup(&archetype));
      hoistResourceReads(rewriter, loops, world);
      rewriter.setInsertionPoint(rootsAt);
    }
  };
  bool followsOrder = inOrder && layout.cascadeFollowsEvents(query);
  if (matched && inOrder && !followsOrder) {
    rewriter.setInsertionPoint(query);
    LinkedTree tree(rewriter, loc, world, relation);
    Walk walk = buildWalk(tree);
    Value zero = tree.zero, one = tree.one;
    Value out = walk.out, outParents = walk.outParents, sizes = walk.sizes;
    Value from = walk.from, count = walk.count;
    Value none = world.noEntity(loc);
    if (!leavesFirst)
      visitAlone(tree, sizes);
    // (Breadth first, children first: the order from its end.)
    bool backwards = leavesFirst && !depthFirst;
    auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(loop.getBody()->getTerminator());
      Value at = loop.getInductionVar();
      if (backwards)
        at = arith::SubIOp::create(
            rewriter, loc, arith::SubIOp::create(rewriter, loc, count, one),
            at);
      at = arith::AddIOp::create(rewriter, loc, from, at);
      Value id = tree.load(out, at);
      Value parent = tree.load(outParents, at);
      Value isRoot = tree.same(parent, none);
      emitLocate(
          rewriter, loc, layout, world, id,
          [&](const WorldArchetype &archetype) {
            return matches(archetype, query);
          },
          FlatSymbolRefAttr(), TypeRange{},
          [&](const WorldArchetype &archetype, Value row,
              Value) -> SmallVector<Value> {
            OpBuilder::InsertionGuard inner(rewriter);
            auto body = [&](KnownParent known) {
              emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                            layout, row, world.count(loc, archetype), tick,
                            seen, /*parallel=*/false, /*directApplies=*/true,
                            known, marks);
            };
            auto branch = scf::IfOp::create(rewriter, loc, isRoot,
                                            /*withElseRegion=*/true);
            if (depthFirst && visitsRoots) {
              rewriter.setInsertionPointToStart(branch.thenBlock());
              body(KnownParent());
            }
            rewriter.setInsertionPointToStart(branch.elseBlock());
            body(KnownParent{&relation, parent});
            return {};
          },
          []() -> SmallVector<Value> { return {}; });
    }
    hoistResourceReads(rewriter, loop, world);
    rewriter.setInsertionPoint(query);
    if (leavesFirst) {
      visitAlone(tree, sizes);
      if (!depthFirst)
        emitRoots();
    }
  } else if (matched && (sorted || !levelled.empty()) && !followsOrder) {
    goThroughRows();
  } else if (matched) {
    rewriter.setInsertionPoint(query);
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    LinkedTree tree(rewriter, loc, world, relation);
    // The order gone by: the tree's list, or, for a query that follows
    // its events in an order asked for exactly, that order (from `base`,
    // `count` of them; the list's entities' parents are next to them, and
    // so are the order's, where an entity without a parent has none).
    Walk walk;
    if (followsOrder)
      walk = buildWalk(tree);
    Value none = world.noEntity(loc);
    Value base = followsOrder ? walk.from : zero;
    Value count =
        followsOrder
            ? walk.count
            : world.toIndex(
                  loc, memref::LoadOp::create(rewriter, loc,
                                              world.treeOrderCount(relation),
                                              ValueRange{zero}));
    Value limit =
        followsOrder
            ? arith::AddIOp::create(rewriter, loc, base, count).getResult()
            : count;
    Value orderIds = followsOrder ? walk.out : world.treeOrder(relation);
    Value orderParents =
        followsOrder ? walk.outParents : world.treeOrderParents(relation);
    // (From the end: the list for children first; an order worked out is
    // made that way, but for breadth first.)
    bool reverse = followsOrder ? leavesFirst && !depthFirst : leavesFirst;
    // Whether the entity with the key `key` is in the order, and where.
    auto placeOf = [&](Value key) -> std::pair<Value, Value> {
      if (followsOrder) {
        Value place = tree.load(world.walkPlaces(relation), key);
        return {arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::sge,
                                      place, tree.i64(0)),
                arith::IndexCastOp::create(rewriter, loc,
                                           rewriter.getIndexType(), place)};
      }
      Value link = tree.load(tree.position(), key);
      return {tree.negate(tree.isNone(link)),
              arith::SubIOp::create(rewriter, loc, world.toIndex(loc, link),
                                    one)};
    };
    // The entities with a mark are the ones a query that follows events
    // has yet to look at (see WorldLayout::cascadeFollowsEvents): a bit
    // per place of the order.
    auto markAt = [&](Value position) {
      markBit(rewriter, loc, world.treeMarks(relation), position);
    };
    // The entity with the key `key`, if it is in the order.
    auto markKey = [&](Value key) {
      auto [there, at] = placeOf(key);
      tree.branch(there, [&, at = at] { markAt(at); });
    };
    // The sibling after the entity with the key `key`, or the one before.
    auto markSibling = [&](Value key, bool after) {
      // (Without links: the one the sort found there.)
      if (!relation.linked) {
        Value sibling = tree.load(world.siblingIds(relation, after), key);
        tree.branch(tree.negate(tree.same(sibling, none)),
                    [&] { markKey(world.entityKey(loc, sibling)); });
        return;
      }
      Value link = tree.load(
          after ? tree.nextSibling() : tree.previousSibling(), key);
      tree.branch(tree.negate(tree.isNone(link)), [&] {
        markKey(arith::SubIOp::create(rewriter, loc,
                                      world.toIndex(loc, link), one));
      });
    };
    // The entity `id`, which is alive: in the list, or, having no parent,
    // by its row.
    bool follows = layout.cascadeFollowsEvents(query);
    bool toRoots = layout.cascadeFollowsToRoots(query);
    // What the body adds into, up the tree.
    llvm::SmallSetVector<Type, 2> addsInto;
    query.getBody().walk([&](CombineOp combine) {
      addsInto.insert(combine.getRef().getType());
    });
    auto markEntityHere = [&](Value id) {
      auto [there, at] = placeOf(world.entityKey(loc, id));
      tree.branch(
          there, [&, at = at] { markAt(at); },
          [&] {
            if (!toRoots)
              return;
            emitLocate(
                rewriter, loc, layout, world, id,
                [&](const WorldArchetype &archetype) {
                  return matches(archetype, query);
                },
                FlatSymbolRefAttr(), TypeRange{},
                [&](const WorldArchetype &archetype, Value row,
                    Value) -> SmallVector<Value> {
                  markBit(rewriter, loc, world.rowMarks(archetype), row);
                  return {};
                },
                []() -> SmallVector<Value> { return {}; });
          });
    };
    auto partNamed = [&](StringRef what) {
      return shares ? partName + "." + what.str() : std::string();
    };
    auto markEntity = [&](Value id) {
      emitPart(rewriter, loc, partNamed("mark"), [&] { markEntityHere(id); });
    };
    // The parent of the entity `id`, which may be no more: if the list
    // has it, with its parent next to it.
    auto markParentOfHere = [&](Value id) {
      auto [there, at] = placeOf(world.entityKey(loc, id));
      tree.branch(
          tree.both(there,
                    arith::CmpIOp::create(rewriter, loc,
                                          arith::CmpIPredicate::ult, at,
                                          limit)),
          [&, at = at] {
            tree.branch(tree.same(tree.load(orderIds, at), id), [&] {
              Value above = tree.load(orderParents, at);
              tree.branch(tree.negate(tree.same(above, none)),
                          [&] { markEntity(above); });
            });
          });
    };
    auto markParentOf = [&](Value id) {
      emitPart(rewriter, loc, partNamed("above"),
               [&] { markParentOfHere(id); });
    };
    // The children of the entity with the key `key` in `along` (a tree):
    // `each` for the key and the id of every one. By the tree's links, or
    // without them the sources of the edges to it.
    auto forChildren = [&](const WorldRelation &along, Value key,
                           function_ref<void(Value, Value)> each) {
      if (!along.linked) {
        auto [begin, end] =
            emitEdgeRange(rewriter, loc, world, along, /*in=*/true, key);
        tree.forEach(begin, end, [&](Value position) {
          Value edge =
              along.isSorted(/*in=*/true)
                  ? position
                  : world.toIndex(loc, tree.load(world.indexEdges(along),
                                                 position));
          Value child =
              tree.load(world.edgeIds(along, /*source=*/true), edge);
          each(world.entityKey(loc, child), child);
        });
        return;
      }
      LinkedTree links(rewriter, loc, world, along);
      Value first = links.load(links.firstChild(), key);
      scf::WhileOp::create(
          rewriter, loc, TypeRange{first.getType()}, ValueRange{first},
          [&](OpBuilder &, Location, ValueRange state) {
            scf::ConditionOp::create(rewriter, loc,
                                     links.negate(links.isNone(state[0])),
                                     state);
          },
          [&](OpBuilder &, Location, ValueRange state) {
            Value child = arith::SubIOp::create(
                rewriter, loc, world.toIndex(loc, state[0]), one);
            {
              OpBuilder::InsertionGuard inner(rewriter);
              each(child, links.sourceOf(child));
            }
            scf::YieldOp::create(
                rewriter, loc,
                ValueRange{links.load(links.nextSibling(), child)});
          });
    };
    // Whether the entity `id` is alive and has all of `has`.
    auto hasAll = [&](Value id, ArrayRef<FlatSymbolRefAttr> has) -> Value {
      Value all = tree.i1(true);
      for (FlatSymbolRefAttr component : has)
        all = tree.both(
            all,
            emitLocate(
                rewriter, loc, layout, world, id,
                [](const WorldArchetype &) { return true; }, component,
                TypeRange{rewriter.getI1Type()},
                [&](const WorldArchetype &home, Value,
                    Value present) -> SmallVector<Value> {
                  if (!ArchetypeOp(home.op).contains(component))
                    return {tree.i1(false)};
                  return {present ? present : tree.i1(true)};
                },
                [&]() -> SmallVector<Value> { return {tree.i1(false)}; })[0]);
      return all;
    };
    // The entities below the one with the key `key` in `along` (a tree
    // with links) whose nearest ancestor with all of `has` it is, or
    // would be if it had them: its children, and those of every one that
    // has not all of them itself. Child by child, down where there is
    // more to ask, else on to the next, back up where there is none.
    auto forThoseBelow = [&](const WorldRelation &along, Value key,
                             ArrayRef<FlatSymbolRefAttr> has,
                             function_ref<void(Value, Value)> each) {
      // (Without links: the ones still to be asked for their children
      // wait in a row, each there once.)
      if (!along.linked) {
        Value waiting = world.reachStack(along);
        Type i64 = rewriter.getI64Type();
        tree.store(arith::IndexCastOp::create(rewriter, loc, i64, key),
                   waiting, zero);
        scf::WhileOp::create(
            rewriter, loc, TypeRange{rewriter.getIndexType()},
            ValueRange{one},
            [&](OpBuilder &, Location, ValueRange state) {
              scf::ConditionOp::create(
                  rewriter, loc,
                  arith::CmpIOp::create(rewriter, loc,
                                        arith::CmpIPredicate::ne, state[0],
                                        zero),
                  state);
            },
            [&](OpBuilder &, Location, ValueRange state) {
              Value top = arith::SubIOp::create(rewriter, loc, state[0], one);
              Value from = arith::IndexCastOp::create(
                  rewriter, loc, rewriter.getIndexType(),
                  tree.load(waiting, top));
              auto [begin, end] = emitEdgeRange(rewriter, loc, world, along,
                                                /*in=*/true, from);
              auto edges = scf::ForOp::create(rewriter, loc, begin, end, one,
                                              ValueRange{top});
              {
                OpBuilder::InsertionGuard inner(rewriter);
                rewriter.setInsertionPointToStart(edges.getBody());
                Value position = edges.getInductionVar();
                Value edge =
                    along.isSorted(/*in=*/true)
                        ? position
                        : world.toIndex(loc,
                                        tree.load(world.indexEdges(along),
                                                  position));
                Value id =
                    tree.load(world.edgeIds(along, /*source=*/true), edge);
                Value child = world.entityKey(loc, id);
                {
                  OpBuilder::InsertionGuard body(rewriter);
                  each(child, id);
                }
                Value count = edges.getRegionIterArg(0);
                Value more = tree.negate(hasAll(id, has));
                tree.branch(more, [&] {
                  tree.store(
                      arith::IndexCastOp::create(rewriter, loc, i64, child),
                      waiting, count);
                });
                scf::YieldOp::create(
                    rewriter, loc,
                    ValueRange{arith::SelectOp::create(
                        rewriter, loc, more,
                        arith::AddIOp::create(rewriter, loc, count, one),
                        count)});
              }
              scf::YieldOp::create(rewriter, loc,
                                   ValueRange{edges.getResult(0)});
            });
        return;
      }
      LinkedTree links(rewriter, loc, world, along);
      Type link = world.offsetType(along);
      Value first = links.load(links.firstChild(), key);
      scf::WhileOp::create(
          rewriter, loc, TypeRange{link}, ValueRange{first},
          [&](OpBuilder &, Location, ValueRange state) {
            scf::ConditionOp::create(rewriter, loc,
                                     links.negate(links.isNone(state[0])),
                                     state);
          },
          [&](OpBuilder &, Location, ValueRange state) {
            Value child = arith::SubIOp::create(
                rewriter, loc, world.toIndex(loc, state[0]), one);
            Value id = links.sourceOf(child);
            {
              OpBuilder::InsertionGuard inner(rewriter);
              each(child, id);
            }
            Value down = arith::SelectOp::create(
                rewriter, loc, hasAll(id, has), links.noLink(),
                links.load(links.firstChild(), child));
            auto next = scf::IfOp::create(rewriter, loc, TypeRange{link},
                                          links.negate(links.isNone(down)),
                                          /*withElseRegion=*/true);
            {
              OpBuilder::InsertionGuard inner(rewriter);
              rewriter.setInsertionPointToStart(next.thenBlock());
              scf::YieldOp::create(rewriter, loc, ValueRange{down});
              rewriter.setInsertionPointToStart(next.elseBlock());
              auto climb = scf::WhileOp::create(
                  rewriter, loc, TypeRange{rewriter.getIndexType()},
                  ValueRange{child},
                  [&](OpBuilder &, Location, ValueRange at) {
                    scf::ConditionOp::create(
                        rewriter, loc,
                        links.both(
                            arith::CmpIOp::create(rewriter, loc,
                                                  arith::CmpIPredicate::ne,
                                                  at[0], key),
                            links.isNone(
                                links.load(links.nextSibling(), at[0]))),
                        at);
                  },
                  [&](OpBuilder &, Location, ValueRange at) {
                    scf::YieldOp::create(
                        rewriter, loc,
                        ValueRange{world.entityKey(
                            loc, links.load(links.targets(), at[0]))});
                  });
              Value top = climb.getResult(0);
              scf::YieldOp::create(
                  rewriter, loc,
                  ValueRange{arith::SelectOp::create(
                      rewriter, loc,
                      arith::CmpIOp::create(rewriter, loc,
                                            arith::CmpIPredicate::eq, top,
                                            key),
                      links.noLink(),
                      links.load(links.nextSibling(), top))});
            }
            scf::YieldOp::create(rewriter, loc, ValueRange{next.getResult(0)});
          });
    };
    // The triggers up the tree, by the steps of their ways: the entities
    // whose way's step `step` (0: themselves) is the one with the key
    // `key` get a mark.
    SmallVector<std::pair<SmallVector<TriggerStep, 2>, SmallVector<unsigned>>>
        waysUp;
    for (auto [index, trigger] : llvm::enumerate(triggers)) {
      if (trigger.where != Trigger::Up)
        continue;
      SmallVector<TriggerStep, 2> steps = getSteps(trigger, query);
      auto same = [&](const auto &way) {
        return way.first.size() == steps.size() &&
               llvm::all_of(llvm::zip(way.first, steps), [](auto pair) {
                 auto &[a, b] = pair;
                 return a.nearest == b.nearest && a.tree == b.tree &&
                        a.has == b.has && a.sibling == b.sibling;
               });
      };
      auto *known = llvm::find_if(waysUp, same);
      if (known == waysUp.end()) {
        waysUp.push_back({steps, {}});
        known = &waysUp.back();
      }
      known->second.push_back(index);
    }
    std::function<void(ArrayRef<TriggerStep>, unsigned, Value)> reach,
        reachHere;
    reach = [&](ArrayRef<TriggerStep> steps, unsigned step, Value key) {
      if (step == 0) {
        markKey(key);
        return;
      }
      emitPart(rewriter, loc, partNamed("reach"),
               [&] { reachHere(steps, step, key); });
    };
    reachHere =
        [&](ArrayRef<TriggerStep> steps, unsigned step, Value key) {
          const TriggerStep &last = steps[step - 1];
          const WorldRelation &along = layout.getRelation(last.tree.getAttr());
          auto further = [&](Value child, Value) {
            reach(steps, step - 1, child);
          };
          // (To the sibling after: so from the one before it.)
          if (last.sibling) {
            bool after = last.sibling < 0;
            if (!along.linked) {
              Value sibling =
                  tree.load(world.siblingIds(along, after), key);
              tree.branch(tree.negate(tree.same(sibling, none)), [&] {
                reach(steps, step - 1, world.entityKey(loc, sibling));
              });
              return;
            }
            LinkedTree links(rewriter, loc, world, along);
            Value link = links.load(
                after ? links.nextSibling() : links.previousSibling(), key);
            links.branch(links.negate(links.isNone(link)), [&] {
              reach(steps, step - 1,
                    arith::SubIOp::create(rewriter, loc,
                                          world.toIndex(loc, link), one));
            });
            return;
          }
          if (last.nearest)
            forThoseBelow(along, key, last.has, further);
          else
            forChildren(along, key, further);
        };
    // Those whose way goes along the edge of the entity with the key
    // `key` in `along`, or past that entity where it has not got what a
    // step asks for (`passed`, with `component` one of those).
    auto reachAlong = [&](StringAttr along, Value key, bool passed,
                          FlatSymbolRefAttr component) {
      for (auto &[steps, which] : waysUp)
        for (unsigned step = 1; step <= steps.size(); ++step) {
          const TriggerStep &at = steps[step - 1];
          if (passed ? !(at.nearest && llvm::is_contained(at.has, component))
                     : at.tree.getAttr() != along)
            continue;
          // (Its own way, from this step on.)
          if (!passed)
            reach(steps, step - 1, key);
          if (at.nearest)
            forThoseBelow(layout.getRelation(at.tree.getAttr()), key, at.has,
                          [&, steps = ArrayRef<TriggerStep>(steps)](
                              Value child, Value) {
                            reach(steps, step - 1, child);
                          });
        }
    };
    // The body for the entity `id` at `row` of `archetype`, for a query
    // that follows its events, and what follows from what it changed.
    std::function<void(const WorldArchetype &, Value, Value, KnownParent)>
        runAtHere;
    auto runAt = [&](const WorldArchetype &archetype, Value row, Value id,
                     KnownParent parent) {
      emitPart(rewriter, loc, partNamed("run"),
               [&] { runAtHere(archetype, row, id, parent); });
    };
    runAtHere = [&](const WorldArchetype &archetype, Value row, Value id,
                    KnownParent parent) {
      // Following events: what the body changed here that a trigger
      // up the tree means (its stamp has this query's tick, which no
      // other event has) is an event for the children, which come
      // later in the list.
      auto ran = scf::IfOp::create(
          rewriter, loc, arith::ConstantIntOp::create(rewriter, loc, 1, 1));
      {
        OpBuilder::InsertionGuard body(rewriter);
        rewriter.setInsertionPointToStart(ran.thenBlock());
        bodyAt(archetype, row, parent);
      }
      // What the body added into the parent is an event of the
      // parent's, which comes later: for itself, or for the one it
      // is under in turn, which it passes it on to when it is come
      // to.
      // (Or into one further up: found as the body found it.)
      if (leavesFirst && parent.relation) {
        auto holds = [&](const WorldArchetype &home) {
          return llvm::any_of(triggers, [&](const Trigger &trigger) {
            return home.findStamp(getStamp(trigger)) != nullptr;
          });
        };
        // Whether the entity `above` has an event of this run.
        auto markIfChanged = [&](Value above) {
          Value got = emitLocate(
              rewriter, loc, layout, world, above, holds,
              FlatSymbolRefAttr(), TypeRange{rewriter.getI1Type()},
              [&](const WorldArchetype &home, Value at,
                  Value) -> SmallVector<Value> {
                Value any = tree.i1(false);
                for (const Trigger &trigger : triggers)
                  if (const WorldColumn *column =
                          home.findStamp(getStamp(trigger)))
                    any = arith::OrIOp::create(
                        rewriter, loc, any,
                        tree.same(tree.load(world.stamps(home, *column),
                                            at),
                                  tick));
                return {any};
              },
              [&]() -> SmallVector<Value> { return {tree.i1(false)}; })[0];
          tree.branch(got, [&] { markEntity(above); });
        };
        bool toParent = false;
        for (Type type : addsInto) {
          auto ref = cast<RefType>(type);
          if (ref.getHops() == 1 &&
              (ref.getIsDirect() ||
               relation.getTrusted(/*target=*/true) == ref.getComponent())) {
            toParent = true;
            continue;
          }
          Ancestor further =
              emitAncestor(rewriter, loc, layout, world, relation,
                           ref.getComponent(), id, Value(), ref.getIsDirect(),
                           ref.getHops());
          Value there = further.found ? further.found : tree.i1(true);
          tree.branch(there, [&] {
            Value above = further.id;
            if (!above)
              above = emitAtHome(
                  rewriter, loc, layout, further, TypeRange{world.idType()},
                  [&](const WorldArchetype &home) -> SmallVector<Value> {
                    return {world.entityId(loc, home, further.row)};
                  })[0];
            markIfChanged(above);
          });
        }
        if (toParent)
          markIfChanged(parent.id);
      }
      // (And for the sibling after, where a trigger is before the
      // tree, and for the parent, which comes later from the leaves,
      // where one is down it. The sibling before has been visited.)
      SmallVector<std::pair<Trigger::Where, unsigned>, 4> ways;
      for (unsigned way = 0; way < waysUp.size(); ++way)
        ways.push_back({Trigger::Up, way});
      ways.push_back({Trigger::Before, 0});
      ways.push_back({Trigger::Down, 0});
      for (auto [where, way] : ways) {
        Value changedHere;
        for (auto [index, trigger] : llvm::enumerate(triggers)) {
          const WorldColumn *column =
              trigger.via && trigger.where == where &&
                      (where != Trigger::Up ||
                       llvm::is_contained(waysUp[way].second, index))
                  ? archetype.findStamp(getStamp(trigger))
                  : nullptr;
          if (!column)
            continue;
          Value now = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::eq,
              memref::LoadOp::create(rewriter, loc,
                                     world.stamps(archetype, *column),
                                     ValueRange{row}),
              tick);
          changedHere = changedHere
                            ? arith::OrIOp::create(rewriter, loc,
                                                   changedHere, now)
                                  .getResult()
                            : now;
        }
        if (!changedHere)
          continue;
        tree.branch(changedHere, [&] {
          if (where == Trigger::Up)
            reach(waysUp[way].first, waysUp[way].first.size(),
                  world.entityKey(loc, id));
          else if (where == Trigger::Before)
            markSibling(world.entityKey(loc, id), /*after=*/true);
          else if (parent.relation && leavesFirst)
            markEntity(parent.id);
        });
      }
    };
    // The body for the entity at `at` of the order, at the insertion point
    // (`visitWith`: for the entity and the parent that were found there).
    std::function<void(Value, Value, KnownParent, bool)> visitWith;
    auto visitAt = [&](Value at, bool following) {
      OpBuilder::InsertionGuard guard(rewriter);
      Value id = memref::LoadOp::create(rewriter, loc, orderIds,
                                        ValueRange{at});
      // (A linked tree's list has entries of all ones where an entity has
      // moved to its end.)
      if (relation.linked && !followsOrder) {
        auto ifThere = scf::IfOp::create(
            rewriter, loc,
            arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ne, id,
                                  world.noEntity(loc)));
        rewriter.setInsertionPointToStart(ifThere.thenBlock());
      }
      // The list has each entity's parent next to it, and where ids are
      // not rows, the locations of both.
      KnownParent parent{&relation,
                         memref::LoadOp::create(rewriter, loc, orderParents,
                                                ValueRange{at})};
      // (An order worked out has, depth first, the entities without a
      // parent that have children: visited if the query visits those,
      // with no parent to name.)
      if (followsOrder) {
        tree.branch(
            tree.same(parent.id, none),
            [&] {
              if (visitsRoots)
                visitWith(at, id, KnownParent(), following);
            },
            [&] { visitWith(at, id, parent, following); });
        return;
      }
      visitWith(at, id, parent, following);
    };
    visitWith = [&](Value at, Value id, KnownParent parent, bool following) {
      Value location;
      if (relation.hasLocations() && !followsOrder) {
        location = memref::LoadOp::create(
            rewriter, loc, world.treeOrderLocations(relation, /*parent=*/false),
            ValueRange{at});
        parent.location = memref::LoadOp::create(
            rewriter, loc, world.treeOrderLocations(relation, /*parent=*/true),
            ValueRange{at});
      }
      // The listed entities are sources of edges. Where those cannot die
      // (the relation's sources are trusted, or nothing dies at all) and can
      // only live in one archetype, each is found there without a check.
      RelationOp relationOp = relation.op;
      FlatSymbolRefAttr from = relationOp.getEndpoint(/*target=*/false);
      bool fromTrusted =
          from && relation.getTrusted(/*target=*/false) == from;
      // Or in several, all of which the query matches: then a branch on
      // its archetype tells where, and nothing else is checked.
      auto isHome = [&](const WorldArchetype &archetype) {
        return !from || ArchetypeOp(archetype.op).contains(from);
      };
      // (A linked tree's edges go with a despawned entity: what is listed
      // is alive.)
      bool certain = from ? fromTrusted
                          : relation.linked ||
                                !layout.entities.hasGenerations();
      for (const WorldArchetype &archetype : layout.archetypes)
        certain &= !isHome(archetype) || matches(archetype, query);
      // (An entity without a parent is the source of no edge.)
      certain &= !followsOrder;
      emitLocate(
          rewriter, loc, layout, world, id,
          [&](const WorldArchetype &archetype) {
            return certain ? isHome(archetype) : matches(archetype, query);
          },
          FlatSymbolRefAttr(), TypeRange{},
          [&](const WorldArchetype &archetype, Value row,
              Value) -> SmallVector<Value> {
            OpBuilder::InsertionGuard inner(rewriter);
            if (!following) {
              bodyAt(archetype, row, parent);
              return {};
            }
            runAt(archetype, row, id, parent);
            return {};
          },
          []() -> SmallVector<Value> { return {}; }, LocateBounds(), certain,
          certain ? location : Value());
    };
    auto visitAll = [&] {
      auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
      {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPoint(loop.getBody()->getTerminator());
        Value at = loop.getInductionVar();
        if (reverse)
          at = arith::SubIOp::create(
              rewriter, loc, arith::SubIOp::create(rewriter, loc, count, one),
              at);
        if (followsOrder)
          at = arith::AddIOp::create(rewriter, loc, base, at);
        visitAt(at, /*following=*/false);
      }
      hoistResourceReads(rewriter, loop, world);
    };
    if (!follows) {
      visitAll();
      if (leavesFirst)
        emitRoots();
    } else {
      // On its first run, and when a log has lost events, every entity as
      // ever. Else from the events: an entity with an event of its own
      // gets a mark, and so do the children of an entity with an event
      // that a trigger up the tree means, the sibling after one with an
      // event that a trigger before the tree means (or the one before,
      // for a trigger after it), and the parent, for a trigger down the
      // tree. Then the marked ones in the list's order, each passing
      // marks on to those if the body changed it. (An event marks more
      // entities than the body runs for, which the body's own test of
      // the ticks sorts out.)
      Value scan =
          openLogsToFollow(rewriter, query, layout, world, seen, count);
      // That an entity has other children or siblings than it had is in
      // a ring the tree keeps of those: all are asked where it has lost
      // one since the query last ran.
      bool sideways = llvm::any_of(waysUp, [](const auto &way) {
        return llvm::any_of(way.first, [](const TriggerStep &step) {
          return step.sibling != 0;
        });
      });
      bool touches = relation.touchedOffset &&
                     (sideways ||
                      llvm::any_of(triggers, [](const Trigger &trigger) {
                        return trigger.where == Trigger::Down ||
                               trigger.where == Trigger::Before ||
                               trigger.where == Trigger::After;
                      }));
      if (touches)
        scan = arith::OrIOp::create(
            rewriter, loc, scan,
            arith::CmpIOp::create(
                rewriter, loc, arith::CmpIPredicate::sgt,
                tree.load(world.touchedState(relation), one), seen));
      auto scanOrFollow = scf::IfOp::create(rewriter, loc, scan,
                                            /*withElseRegion=*/true);
      rewriter.setInsertionPointToStart(scanOrFollow.thenBlock());
      if (byWalk) {
        // (Everything: a sorted tree by its rows, as ever, those without
        // a parent with it.)
        rowsAt = rootsAt = scanOrFollow.thenBlock()->getTerminator();
        if (!leavesFirst)
          emitRoots();
        goThroughRows();
        rowsAt = rootsAt = query;
      } else {
        // (Parents first, those without a parent before the others.)
        if (toRoots && !leavesFirst) {
          rootsAt = scanOrFollow.thenBlock()->getTerminator();
          if (followsOrder && depthFirst)
            visitAlone(tree, walk.sizes);
          else
            emitRoots();
          rootsAt = query;
          rewriter.setInsertionPoint(
              scanOrFollow.thenBlock()->getTerminator());
        }
        visitAll();
      }
      rewriter.setInsertionPointToStart(scanOrFollow.elseBlock());
      // The ones with other children or siblings since: themselves.
      if (touches) {
        Value last = arith::ConstantIndexOp::create(
            rewriter, loc, WorldRelation::kTouched - 1);
        Value ever = world.toIndex(
            loc, tree.load(world.touchedState(relation), zero));
        Value kept = arith::MinUIOp::create(
            rewriter, loc, ever,
            arith::ConstantIndexOp::create(rewriter, loc,
                                           WorldRelation::kTouched));
        tree.forEach(zero, kept, [&](Value back) {
          Value slot = arith::AndIOp::create(
              rewriter, loc,
              arith::SubIOp::create(
                  rewriter, loc, arith::SubIOp::create(rewriter, loc, ever, one),
                  back),
              last);
          tree.branch(
              arith::CmpIOp::create(
                  rewriter, loc, arith::CmpIPredicate::sgt,
                  tree.load(world.touchedTicks(relation), slot), seen),
              [&] {
                Value touched = tree.load(world.touchedIds(relation), slot);
                markEntity(touched);
                // (And those whose way goes on from it to a sibling.)
                for (auto &[steps, which] : waysUp)
                  for (unsigned step = 1; step <= steps.size(); ++step)
                    if (steps[step - 1].sibling)
                      reach(steps, step - 1, world.entityKey(loc, touched));
              });
        });
      }
      forEachEvent(rewriter, query, layout, world,
                   [&](const Trigger &trigger, Value id) {
                     Value key = world.entityKey(loc, id);
                     // (An entity that lost or got what a `*` asks for:
                     // those that went past it, or stop at it now.)
                     if (trigger.onTheWay) {
                       reachAlong(StringAttr(), key, /*passed=*/true,
                                  trigger.component);
                       return;
                     }
                     switch (trigger.where) {
                     case Trigger::Own:
                       if (toRoots) {
                         // (An entity in no list may be no more.)
                         tree.branch(
                             placeOf(key).first, [&] { markKey(key); },
                             [&] {
                               emitLocate(
                                   rewriter, loc, layout, world, id,
                                   [&](const WorldArchetype &archetype) {
                                     return matches(archetype, query);
                                   },
                                   FlatSymbolRefAttr(), TypeRange{},
                                   [&](const WorldArchetype &archetype,
                                       Value row,
                                       Value) -> SmallVector<Value> {
                                     markBit(rewriter, loc,
                                             world.rowMarks(archetype), row);
                                     return {};
                                   },
                                   []() -> SmallVector<Value> { return {}; });
                             });
                       } else {
                         markKey(key);
                       }
                       // (Connected to another parent: those whose way
                       // up goes along that edge see another too.)
                       if (trigger.kind == Trigger::Connected)
                         reachAlong(trigger.component.getAttr(), key,
                                    /*passed=*/false, FlatSymbolRefAttr());
                       break;
                     case Trigger::Up: {
                       SmallVector<TriggerStep, 2> steps =
                           getSteps(trigger, query);
                       reach(steps, steps.size(), key);
                       break;
                     }
                     case Trigger::Down:
                       markParentOf(id);
                       break;
                     case Trigger::Before:
                       markSibling(key, /*after=*/true);
                       break;
                     case Trigger::After:
                       markSibling(key, /*after=*/false);
                       break;
                     }
                   });
      // Parents first, the marked ones without a parent come first, by
      // their rows: what the body changes in one is an event for those
      // below it.
      if (toRoots && !leavesFirst)
        for (const WorldArchetype &archetype : layout.archetypes) {
          if (!matches(archetype, query))
            continue;
          scf::ForOp roots = sweepMarks(
              rewriter, loc, world.rowMarks(archetype),
              startCounts.lookup(&archetype), [&](Value row) {
                runAt(archetype, row, world.entityId(loc, archetype, row),
                      KnownParent());
              });
          hoistResourceReads(rewriter, roots, world);
        }
      scf::ForOp sweep =
          (reverse ? sweepMarksDown : sweepMarks)(
              rewriter, loc, world.treeMarks(relation), limit,
              [&](Value at) { visitAt(at, /*following=*/true); });
      hoistResourceReads(rewriter, sweep, world);
      // Then the marked ones without a parent, by their rows.
      if (toRoots && leavesFirst)
        for (const WorldArchetype &archetype : layout.archetypes) {
          if (!matches(archetype, query))
            continue;
          scf::ForOp roots = sweepMarks(
              rewriter, loc, world.rowMarks(archetype),
              startCounts.lookup(&archetype), [&](Value row) {
                bodyAt(archetype, row, KnownParent());
              });
          hoistResourceReads(rewriter, roots, world);
        }
      rewriter.setInsertionPoint(query);
      closeLogs(rewriter, query, layout, world);
      if (leavesFirst && !byWalk) {
        // (Everything: so too the ones without a parent. Depth first
        // those with children were in the order.)
        auto ifScan = scf::IfOp::create(rewriter, loc, scan);
        rootsAt = ifScan.thenBlock()->getTerminator();
        if (followsOrder && depthFirst)
          visitAlone(tree, walk.sizes);
        else
          emitRoots();
        rootsAt = query;
        rewriter.setInsertionPoint(query);
      }
    }
  } else {
    query.emitWarning("matches no archetype; the query is removed");
  }
  // The marked rows, in order, are the pending list; then the query's end
  // as any query's.
  rewriter.setInsertionPoint(query);
  SmallVector<const WorldArchetype *> changed;
  for (auto [archetype, rows] : despawning) {
    changed.push_back(archetype);
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    Value list = world.pendingList(*archetype);
    auto gather = scf::ForOp::create(rewriter, loc, zero, rows, one,
                                     ValueRange{zero});
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(gather.getBody());
      Value row = gather.getInductionVar();
      Value at = gather.getRegionIterArg(0);
      Value marked = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::ne,
          memref::LoadOp::create(rewriter, loc, list, ValueRange{row}),
          arith::ConstantIntOp::create(rewriter, loc, 0, 32));
      auto ifMarked = scf::IfOp::create(rewriter, loc, marked);
      rewriter.setInsertionPointToStart(ifMarked.thenBlock());
      memref::StoreOp::create(
          rewriter, loc,
          arith::IndexCastOp::create(rewriter, loc, rewriter.getI32Type(),
                                     row),
          list, ValueRange{at});
      // Its action and a move's values come along, from the row's place
      // to the list's.
      SmallVector<Value> carried;
      if (archetype->pendingActionOffset)
        carried.push_back(world.pendingActions(*archetype));
      for (const WorldMove &move : archetype->moves)
        for (const WorldColumn &column : move.values)
          carried.push_back(world.moveValues(*archetype, column));
      for (Value column : carried)
        memref::StoreOp::create(
            rewriter, loc,
            memref::LoadOp::create(rewriter, loc, column, ValueRange{row}),
            column, ValueRange{at});
      rewriter.setInsertionPointAfter(ifMarked);
      scf::YieldOp::create(
          rewriter, loc,
          ValueRange{arith::SelectOp::create(
              rewriter, loc, marked,
              arith::AddIOp::create(rewriter, loc, at, one), at)});
    }
    memref::StoreOp::create(
        rewriter, loc,
        arith::IndexCastOp::create(rewriter, loc, rewriter.getI64Type(),
                                   gather.getResult(0)),
        world.pendingCount(*archetype), ValueRange{zero});
  }
  // Connected edges are added while the rows that connected them are
  // where they were, before despawns and moves.
  for (ConnectOp connect : connects)
    for (const WorldArchetype &archetype : layout.archetypes)
      if (Value rows = startCounts.lookup(&archetype))
        appendConnected(rewriter, connect, layout, archetype, world, rows);
  // A query that reacts to its ancestors' events has passed down the ones
  // it caused itself. The counter moves on, so that these are the only
  // events of their tick and the next time can tell them from what came
  // after.
  if (llvm::any_of(triggers, [](const Trigger &trigger) {
        return static_cast<bool>(trigger.via);
      })) {
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    memref::StoreOp::create(rewriter, loc, world.currentTick(loc),
                            world.tickCounter(), ValueRange{zero});
  }
  commitStructure(rewriter, loc, layout, world, changed, changedRelations,
                  tick);
  rewriter.eraseOp(query);
}

/// Replace a query by one loop per matching archetype. The body is cloned
/// into each loop, and every ref access becomes a load or store at the
/// loop's index in the column of the ref's component and field.
///
/// A reactive query whose triggers all have event logs walks them instead,
/// unless this is its first run or more events happened since it last read
/// a log than the log holds; then it scans every entity, keeping the
/// effects where a trigger's stamp is newer.
static void lowerQuery(IRRewriter &rewriter, QueryOp query,
                       const WorldLayout &layout, WorldAccess &world,
                       const LoopOptions &options) {
  if (query.getCascade())
    return lowerCascade(rewriter, query, layout, world, options);
  Location loc = query.getLoc();
  bool entityLocal = isEntityLocal(query);
  bool matched = false;
  SmallVector<const WorldArchetype *> changed;
  // A reactive query starts by taking the tick it last started at (0 at
  // first, so every stamp counts) and advancing the counter: events from
  // here on, its own included, are stamped later than its new last tick.
  SmallVector<Trigger> triggers = getTriggers(query);
  Value seen;
  rewriter.setInsertionPoint(query);
  if (!triggers.empty()) {
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value last = world.lastTick(query);
    seen = memref::LoadOp::create(rewriter, loc, last, ValueRange{zero});
    Value next = world.currentTick(loc);
    memref::StoreOp::create(rewriter, loc, next, last, ValueRange{zero});
    memref::StoreOp::create(rewriter, loc, next, world.tickCounter(),
                            ValueRange{zero});
  }
  // The tick this query's events are stamped with; constant while it runs.
  Value tick = world.hasStamps() ? world.currentTick(loc) : Value();

  // Event logs: read where each ends and where this query last read it;
  // scan on the first run or if a log was overwritten since.
  bool useLogs = false;
  if (!triggers.empty()) {
    std::optional<std::string> reason = whyScans(query, layout);
    useLogs = !reason;
    if (reason && options.explain)
      query.emitRemark("scans every entity on each run: ") << *reason;
  }
  auto index = query->getAttrOfType<IntegerAttr>(
      WorldLayout::kReactiveIndexAttr);
  scf::IfOp scanOrWalk;
  Operation *anchor = query;
  Value zero, one;
  // Entries pending in all the query's logs, for choosing a parallel walk.
  Value pending;
  if (useLogs) {
    zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    Value scan;
    std::tie(scan, pending) = openLogs(rewriter, query, layout, world, seen);
    scanOrWalk = scf::IfOp::create(rewriter, loc, scan,
                                   /*withElseRegion=*/true);
    anchor = scanOrWalk.thenBlock()->getTerminator();
  }

  SmallVector<ApplyOp> applies;
  query.getBody().walk([&](ApplyOp apply) { applies.push_back(apply); });
  SmallVector<AccumulateOp> accumulates;
  query.getBody().walk(
      [&](AccumulateOp accumulate) { accumulates.push_back(accumulate); });
  SmallVector<ConnectOp> connects;
  query.getBody().walk([&](ConnectOp connect) { connects.push_back(connect); });
  // Relations whose edges the query changes: sorted when it ends.
  llvm::SetVector<Attribute> changedRelations;
  noteOrderWrites(rewriter, query, layout, world, changedRelations);
  for (ConnectOp connect : connects)
    changedRelations.insert(connect.getRelationAttr().getAttr());
  query.getBody().walk([&](DisconnectOp disconnect) {
    changedRelations.insert(
        disconnect->getParentOfType<EdgesOp>().getRelationAttr().getAttr());
  });
  // And a sorted archetype it spawns into, which the spawn marks.
  query.getBody().walk([&](SpawnOp spawn) {
    for (const WorldArchetype &archetype : layout.archetypes)
      if (archetype.isSorted() &&
          ArchetypeOp(archetype.op).getSymNameAttr() ==
              spawn.getArchetypeAttr().getAttr())
        changedRelations.insert(archetype.sortedBy);
  });
  // A query visits the entities that exist when it starts: count every
  // matched archetype now, before any of its loops, since a body may spawn
  // into an archetype whose loop comes later. The same counts bound the
  // rows whose applies are combined.
  SmallVector<std::pair<const WorldArchetype *, Value>> visited;
  // Archetypes whose loop combined its unobserved applies directly.
  llvm::SmallPtrSet<const WorldArchetype *, 4> direct;
  llvm::DenseMap<const WorldArchetype *, Value> startCounts;
  {
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPoint(scanOrWalk ? scanOrWalk.getOperation()
                                          : query.getOperation());
    for (const WorldArchetype &archetype : layout.archetypes)
      if (matches(archetype, query))
        startCounts[&archetype] = world.count(loc, archetype);
  }
  for (const WorldArchetype &archetype : layout.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    // No trigger can fire for the entities of this archetype.
    if (!triggers.empty() &&
        llvm::none_of(triggers, [&](const Trigger &trigger) {
          return trigger.via || archetype.findStamp(getStamp(trigger));
        }))
      continue;
    rewriter.setInsertionPoint(anchor);
    Value rows = startCounts.lookup(&archetype);
    if (!applies.empty() || !accumulates.empty() || !connects.empty())
      visited.push_back({&archetype, rows});
    // A loop that never runs in parallel visits the entities in the order
    // applies are combined in, so it may combine unobserved ones directly.
    bool sequentialOnly = options.directApplies &&
                          (!options.parallelEntities || !entityLocal ||
                           archetype.capacity < options.parallelMinEntities);
    if (sequentialOnly)
      direct.insert(&archetype);
    // (A reactive query's body is a part: the same for every archetype
    // but for its columns, and one function for all then. Not where it
    // sends values to be combined, which the loop around it carries.)
    bool asPart = !triggers.empty() && applies.empty() &&
                  accumulates.empty() && connects.empty();
    Operation *loops = emitEntityLoops(
        rewriter, loc, archetype, world, options, entityLocal,
        [&](Value entity, Value rows, bool parallel) {
          emitPart(rewriter, loc, asPart && !parallel ? "body" : "", [&] {
            emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                          layout, entity, rows, tick, seen, parallel,
                          sequentialOnly);
          });
        },
        rows);
    // Directly combined accumulates: their resource cells in registers.
    if (auto loop = dyn_cast<scf::ForOp>(loops); loop && sequentialOnly) {
      SmallVector<Value> cells;
      for (AccumulateOp accumulate : accumulates)
        if (appliesDirectly(accumulate, true))
          cells.push_back(
              world.resourceField(accumulate.getResourceAttr().getAttr(),
                                  accumulate.getFieldAttr()));
      if (!cells.empty())
        loops = carryCells(rewriter, loop, cells);
    }
    hoistResourceReads(rewriter, loops, world);
    if (archetype.hasPending() && isStructuralFor(query, archetype.op))
      changed.push_back(&archetype);
  }

  if (useLogs) {
    rewriter.setInsertionPoint(scanOrWalk.elseBlock()->getTerminator());
    // A walk only pays for a parallel region beyond some number of entries;
    // the count of pending entries decides at run time.
    if (options.parallelEntities && entityLocal) {
      Value many = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::sge, pending,
          arith::ConstantIntOp::create(rewriter, loc,
                                       options.parallelMinEvents, 64));
      auto walk = scf::IfOp::create(rewriter, loc, many,
                                    /*withElseRegion=*/true);
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(walk.thenBlock()->getTerminator());
      walkLogs(rewriter, query, layout, world, tick, seen, /*parallel=*/true);
      rewriter.setInsertionPoint(walk.elseBlock()->getTerminator());
      walkLogs(rewriter, query, layout, world, tick, seen, /*parallel=*/false);
    } else {
      walkLogs(rewriter, query, layout, world, tick, seen, /*parallel=*/false);
    }
    rewriter.setInsertionPoint(query);
    closeLogs(rewriter, query, layout, world);
  }

  // Applies are combined when the whole query has run, in a fixed order:
  // by apply, then archetype, then row. They land before despawns and
  // moves, while every id still leads to where its entity was.
  rewriter.setInsertionPoint(query);
  for (ApplyOp apply : applies)
    for (auto [archetype, count] : visited) {
      if (appliesDirectly(apply, direct.contains(archetype)))
        continue;
      if (apply->getParentOfType<EdgesOp>())
        combineEdgeApplied(rewriter, apply, layout, *archetype, world, count,
                           tick);
      else
        combineApplied(rewriter, apply, layout, *archetype, world, count,
                       tick);
    }
  for (AccumulateOp accumulate : accumulates)
    for (auto [archetype, count] : visited)
      if (!appliesDirectly(accumulate, direct.contains(archetype)))
        combineAccumulated(rewriter, accumulate, *archetype, world, count);
  // Connected edges are appended while the buffers' rows still are the
  // rows that filled them, before despawns and moves.
  for (ConnectOp connect : connects)
    for (auto [archetype, count] : visited)
      appendConnected(rewriter, connect, layout, *archetype, world, count);

  // Despawns and moves take effect when the whole query has run, so an
  // entity moved into another archetype the query matches is not visited
  // twice.
  rewriter.setInsertionPoint(query);
  commitStructure(rewriter, loc, layout, world, changed, changedRelations,
                  tick);

  // The set of archetypes is closed, so a query that matches none of them
  // can never run; that is almost certainly a mistake in the program.
  if (!matched)
    query.emitWarning("matches no archetype; the query is removed");
  rewriter.eraseOp(query);
}

/// Inline the systems of `runs` and fuse their queries: one loop per
/// archetype, holding, in program order, the body of every query of every
/// run that matches it. Emitted before `insertionPoint`; the runs are
/// erased.
///
/// This is legal for any sequence of entity-local systems, conflicting or
/// not: a query body only touches the components of its own entity, and
/// different archetypes share no columns, so running all bodies for one
/// entity before moving on gives the same result as running each query to
/// completion in turn.
static void fuseRuns(IRRewriter &rewriter, MutableArrayRef<RunOp> runs,
                     Operation *insertionPoint, SymbolTable &symbols,
                     const WorldLayout &layout, WorldAccess &world,
                     const LoopOptions &options) {
  if (runs.empty())
    return;
  Location loc = insertionPoint->getLoc();
  rewriter.setInsertionPoint(insertionPoint);

  // Per run: map the system's parameters to the run's arguments and clone
  // the system's ops outside queries, which are free of effects.
  SmallVector<SystemOp> systems;
  SmallVector<IRMapping> mappings(runs.size());
  for (auto [run, mapping] : llvm::zip(runs, mappings)) {
    auto system = symbols.lookup<SystemOp>(run.getSystem());
    systems.push_back(system);
    mapping.map(system.getBody().getArguments(), run.getArgs());
    for (Operation &op : system.getBody().front().without_terminator())
      if (!isa<QueryOp>(op))
        rewriter.clone(op, mapping);
  }

  // No reactive query is fused, so the counter cannot change while the
  // fused loops run.
  rewriter.setInsertionPoint(insertionPoint);
  Value tick = world.hasStamps() ? world.currentTick(loc) : Value();
  for (const WorldArchetype &archetype : layout.archetypes) {
    SmallVector<std::pair<QueryOp, unsigned>> bodies;
    for (auto [index, system] : llvm::enumerate(systems))
      for (QueryOp query : system.getBody().getOps<QueryOp>())
        if (matches(archetype, query))
          bodies.push_back({query, index});
    if (bodies.empty())
      continue;

    rewriter.setInsertionPoint(insertionPoint);
    Operation *loops = emitEntityLoops(
        rewriter, loc, archetype, world, options, /*entityLocal=*/true,
        [&](Value entity, Value rows, bool parallel) {
          for (auto [query, index] : bodies)
            emitQueryBody(rewriter, query, mappings[index], archetype, world,
                          layout, entity, rows, tick, Value(), parallel);
        });
    hoistResourceReads(rewriter, loops, world);
  }

  for (RunOp run : runs)
    rewriter.eraseOp(run);
}

/// Fuse every maximal sequence of runs of entity-local systems in a
/// schedule. Stages are dissolved first: fusion subsumes them. A run of an
/// opaque system, a run under a condition, or an op with effects in the
/// schedule body, ends a sequence and stays where it is. So does a run of a system that writes a
/// resource: fusion moves system-level code ahead of every query of the
/// sequence, which would let queries see a write that follows them.
static void fuseSchedule(IRRewriter &rewriter, func::FuncOp func,
                         SymbolTable &symbols, const WorldLayout &layout,
                         WorldAccess &world, const LoopOptions &options) {
  for (StageOp stage : llvm::make_early_inc_range(func.getOps<StageOp>())) {
    for (Operation &op : llvm::make_early_inc_range(
             stage.getBody().front().without_terminator()))
      op.moveBefore(stage);
    rewriter.eraseOp(stage);
  }

  SmallVector<ArchetypeOp> archetypes;
  for (const WorldArchetype &archetype : layout.archetypes)
    archetypes.push_back(archetype.op);

  SmallVector<RunOp> sequence;
  for (Operation &op : llvm::make_early_inc_range(func.getBody().front())) {
    if (auto run = dyn_cast<RunOp>(op)) {
      // An extern system has no body to fuse; it ends a sequence too.
      auto system = symbols.lookup<SystemOp>(run.getSystem());
      if (!system) {
        fuseRuns(rewriter, sequence, &op, symbols, layout, world, options);
        sequence.clear();
        continue;
      }
      // A run under a condition runs as a whole or not at all; it is not
      // interleaved with others.
      bool conditional = !run.getCondition().empty();
      // Resource writes, structural changes, lookups, applies and edges
      // end a sequence: fusion interleaves systems per entity, and a lookup
      // would then see some entities' updates from other systems and not
      // others', as would a later system reading what an apply combines;
      // edge loops reach other entities too, and connects sort the edges.
      bool writesResource =
          system
              .walk([](Operation *op) {
                return isa<WriteOp, SpawnOp, DespawnOp, LookupOp, ApplyOp,
                           AccumulateOp, EdgesOp, ConnectOp>(op)
                           ? WalkResult::interrupt()
                           : WalkResult::advance();
              })
              .wasInterrupted();
      // A query in a counted loop runs so many times, in its place.
      system.walk([&](QueryOp query) {
        writesResource |= query->getParentOp() != system.getOperation();
      });
      for (QueryOp query : system.getBody().getOps<QueryOp>()) {
        for (ArchetypeOp archetype : getMatchedArchetypes(query))
          writesResource |= isStructuralFor(query, archetype);
        // A reactive query advances the tick counter when it starts, which
        // fusion would move ahead of the systems before it.
        writesResource |= !getTriggers(query).empty();
        // A ref to an ancestor reads another entity, like a lookup, and a
        // cascading query has an order of its own.
        writesResource |= query.hasUpRefs() || query.getCascade();
      }
      if (!conditional && !writesResource &&
          !computeAccess(system, archetypes).isOpaque()) {
        sequence.push_back(run);
        continue;
      }
    } else if (!op.hasTrait<OpTrait::IsTerminator>() &&
               isMemoryEffectFree(&op)) {
      continue;
    }
    fuseRuns(rewriter, sequence, &op, symbols, layout, world, options);
    sequence.clear();
  }
}

/// The address of a memref as the pointer C code takes it (the world's
/// arena as `ent_world *`).
static Value worldPointer(IRRewriter &rewriter, Location loc, Value arena) {
  Value address =
      memref::ExtractAlignedPointerAsIndexOp::create(rewriter, loc, arena);
  Value bits = arith::IndexCastOp::create(rewriter, loc,
                                          rewriter.getI64Type(), address);
  return LLVM::IntToPtrOp::create(
      rewriter, loc, LLVM::LLVMPointerType::get(rewriter.getContext()), bits);
}

/// A bool crosses to C as a byte.
static Type externParamType(Type type) {
  if (auto named = dyn_cast<EnumType>(type))
    return named.getStorageType();
  return type.isInteger(1) ? IntegerType::get(type.getContext(), 8) : type;
}

/// Declare the C function of an extern system, `void ent_<name>(ent_world
/// *world, params...)`.
static LogicalResult declareExtern(IRRewriter &rewriter, ExternOp external,
                                   SymbolTable &symbols) {
  std::string name = external.getCName();
  if (Operation *other = symbols.lookup(name)) {
    InFlightDiagnostic diag = external.emitError("is called as the C function '")
                              << name << "', a name this program also declares";
    diag.attachNote(other->getLoc()) << "declared here";
    return diag;
  }
  SmallVector<Type> inputs{LLVM::LLVMPointerType::get(rewriter.getContext())};
  for (Type type : external.getParams().getAsValueRange<TypeAttr>())
    inputs.push_back(externParamType(type));
  rewriter.setInsertionPoint(external);
  auto func = func::FuncOp::create(rewriter, external.getLoc(), name,
                                   rewriter.getFunctionType(inputs, {}));
  func.setPrivate();
  return success();
}

/// How a value of `type` crosses to a C function: a bool as a byte, a text
/// as a pointer to it.
static Type functionParamType(Type type) {
  if (isa<TextType>(type))
    return LLVM::LLVMPointerType::get(type.getContext());
  return externParamType(type);
}

/// Declare the C function of an extern fn or proc, `ent_<name>(params...)`,
/// or make the function of a fn with a body, under the same name: its
/// body with its values as they are.
static LogicalResult declareFunction(IRRewriter &rewriter, FunctionOp function,
                                     SymbolTable &symbols) {
  std::string name = function.getCName();
  if (Operation *other = symbols.lookup(name)) {
    InFlightDiagnostic diag =
        function.emitError(function.isDefined()
                               ? "is lowered to the function '"
                               : "is called as the C function '")
        << name << "', a name this program also declares";
    diag.attachNote(other->getLoc()) << "declared here";
    return diag;
  }
  bool defined = function.isDefined();
  SmallVector<Type> inputs, results;
  for (Type type : function.getParams().getAsValueRange<TypeAttr>())
    inputs.push_back(defined ? type : functionParamType(type));
  for (Type result : function.getResultTypes())
    results.push_back(defined ? result : externParamType(result));
  rewriter.setInsertionPoint(function);
  auto func = func::FuncOp::create(rewriter, function.getLoc(), name,
                                   rewriter.getFunctionType(inputs, results));
  func.setPrivate();
  if (!defined)
    return success();
  rewriter.inlineRegionBefore(function.getBody(), func.getBody(),
                              func.getBody().end());
  Operation *terminator = func.getBody().front().getTerminator();
  SmallVector<Value> values(terminator->getOperands());
  rewriter.setInsertionPoint(terminator);
  rewriter.replaceOpWithNewOp<func::ReturnOp>(terminator, values);
  return success();
}

/// Replace `invoke` by a call of the function. One with a body (`defined`)
/// takes the values as they are. For a C function a text argument is put
/// on the stack for the call, which gets its address; the stack is given
/// back right after, since the call may sit in a loop.
static void lowerInvoke(IRRewriter &rewriter, InvokeOp invoke,
                        FunctionOp function, bool defined) {
  Location loc = invoke.getLoc();
  if (defined) {
    rewriter.setInsertionPoint(invoke);
    auto call = func::CallOp::create(rewriter, loc, function.getCName(),
                                     invoke->getResultTypes(),
                                     invoke.getArgs());
    rewriter.replaceOp(invoke, call.getResults());
    return;
  }
  bool hasText = llvm::any_of(invoke.getArgs(), [](Value arg) {
    return isa<TextType>(arg.getType());
  });
  SmallVector<Type> resultTypes;
  if (invoke.getResult())
    resultTypes.push_back(invoke.getResult().getType());

  rewriter.setInsertionPoint(invoke);
  memref::AllocaScopeOp scope;
  if (hasText) {
    scope = memref::AllocaScopeOp::create(rewriter, loc, resultTypes);
    rewriter.createBlock(&scope.getBodyRegion());
  }
  SmallVector<Value> args;
  for (Value arg : invoke.getArgs()) {
    if (auto text = dyn_cast<TextType>(arg.getType())) {
      IntegerType storage = text.getStorageType();
      Value slot = memref::AllocaOp::create(
          rewriter, loc, MemRefType::get({}, storage), ValueRange{},
          rewriter.getI64IntegerAttr(16));
      Value bits =
          UnrealizedConversionCastOp::create(rewriter, loc, storage, arg)
              .getResult(0);
      memref::StoreOp::create(rewriter, loc, bits, slot, ValueRange{});
      args.push_back(worldPointer(rewriter, loc, slot));
    } else if (arg.getType().isInteger(1)) {
      args.push_back(
          arith::ExtUIOp::create(rewriter, loc, rewriter.getI8Type(), arg));
    } else if (auto named = dyn_cast<EnumType>(arg.getType())) {
      // An enum crosses as the byte that numbers its case.
      args.push_back(UnrealizedConversionCastOp::create(
                         rewriter, loc, named.getStorageType(), arg)
                         .getResult(0));
    } else {
      args.push_back(arg);
    }
  }
  SmallVector<Type> callResults;
  for (Type type : resultTypes)
    callResults.push_back(externParamType(type));
  auto call = func::CallOp::create(rewriter, loc, function.getCName(),
                                   callResults, args);
  SmallVector<Value> results;
  if (!resultTypes.empty()) {
    Value result = call.getResult(0);
    if (resultTypes.front().isInteger(1))
      result = arith::TruncIOp::create(rewriter, loc, rewriter.getI1Type(),
                                       result);
    else if (isa<EnumType>(resultTypes.front()))
      result = UnrealizedConversionCastOp::create(rewriter, loc,
                                                  resultTypes.front(), result)
                   .getResult(0);
    results.push_back(result);
  }
  if (scope) {
    memref::AllocaScopeReturnOp::create(rewriter, loc, results);
    results.assign(scope.getResults().begin(), scope.getResults().end());
  }
  rewriter.replaceOp(invoke, results);
}

/// Replace `run` by a call of its system, guarded by its condition if it
/// has one: `scf.execute_region { condition; scf.if %c { call } }`, one op,
/// so that a stage section still holds one op per run. An extern system is
/// called as its C function, with the world first, in such a region too.
static void lowerRun(IRRewriter &rewriter, RunOp run, Value arena,
                     SymbolTable &symbols, const WorldLayout &layout) {
  Location loc = run.getLoc();
  auto external = symbols.lookup<ExternOp>(run.getSystem());
  auto emitCall = [&]() {
    if (!external) {
      SmallVector<Value> args(run.getArgs());
      args.push_back(arena);
      func::CallOp::create(rewriter, loc, run.getSystem(), TypeRange{}, args);
      return;
    }
    SmallVector<Value> args{worldPointer(rewriter, loc, arena)};
    for (Value arg : run.getArgs())
      args.push_back(arg.getType().isInteger(1)
                         ? arith::ExtUIOp::create(rewriter, loc,
                                                  rewriter.getI8Type(), arg)
                               .getResult()
                         : arg);
    func::CallOp::create(rewriter, loc, external.getCName(), TypeRange{},
                         args);
    // Edges it connected (through the header) are sorted before the next
    // system visits them, as after a connect of the program's own.
    ArrayAttr writes = external.getWritesAttr();
    for (const WorldRelation &relation : layout.relations) {
      RelationOp relationOp = relation.op;
      if (!external.hasContract() ||
          (writes && llvm::is_contained(
                         writes, FlatSymbolRefAttr::get(
                                     relationOp.getSymNameAttr()))))
        callSort(rewriter, loc, relation, arena);
    }
  };
  rewriter.setInsertionPoint(run);
  if (run.getCondition().empty() && !external) {
    emitCall();
    rewriter.eraseOp(run);
    return;
  }
  auto region = scf::ExecuteRegionOp::create(rewriter, loc, TypeRange{});
  Block *block = rewriter.createBlock(&region.getRegion());
  if (run.getCondition().empty()) {
    emitCall();
  } else {
    IRMapping mapping;
    for (Operation &op : run.getCondition().front().without_terminator())
      rewriter.clone(op, mapping);
    auto yield = cast<YieldOp>(run.getCondition().front().getTerminator());
    Value condition = mapping.lookupOrDefault(yield.getResults()[0]);
    auto branch = scf::IfOp::create(rewriter, loc, condition);
    rewriter.setInsertionPointToStart(branch.thenBlock());
    emitCall();
  }
  rewriter.setInsertionPointToEnd(block);
  scf::YieldOp::create(rewriter, loc);
  rewriter.eraseOp(run);
}

/// Turn the entry point into the program's `main`: a private function
/// holding the body, where loops become `scf.while` and schedule calls
/// calls of the lowered schedules, and a C `main` that creates the world
/// as the generated header does (the arena, with its header zeroed), runs
/// that function and returns 0.
static LogicalResult lowerMain(IRRewriter &rewriter, MainOp main,
                               ModuleOp module, const WorldLayout &layout,
                               MemRefType arenaType) {
  if (Operation *other = SymbolTable::lookupSymbolIn(module, "main")) {
    InFlightDiagnostic diag =
        main.emitError("lowers to the function 'main', a name this program "
                       "also declares");
    diag.attachNote(other->getLoc()) << "declared here";
    return diag;
  }
  Location loc = main.getLoc();
  auto [body, arena] =
      convertToFunc(rewriter, main, "ent.main", main.getBody(), arenaType);
  body.setPrivate();

  // Innermost first: an outer loop moves the lowered inner ones.
  SmallVector<LoopOp> loops;
  body.walk<WalkOrder::PostOrder>([&](LoopOp loop) { loops.push_back(loop); });
  for (LoopOp loop : loops) {
    rewriter.setInsertionPoint(loop);
    // The body runs in the `before` region, which then decides whether to
    // go on: a loop that tests after each run.
    auto whileOp = scf::WhileOp::create(
        rewriter, loop.getLoc(), TypeRange{}, ValueRange{},
        [](OpBuilder &, Location, ValueRange) {},
        [](OpBuilder &builder, Location loc, ValueRange) {
          scf::YieldOp::create(builder, loc);
        });
    Block *before = whileOp.getBeforeBody();
    auto yield = cast<YieldOp>(loop.getBody().front().getTerminator());
    for (Operation &op :
         llvm::make_early_inc_range(loop.getBody().front().without_terminator()))
      op.moveBefore(before, before->end());
    rewriter.setInsertionPointToEnd(before);
    Value proceed = arith::ConstantIntOp::create(rewriter, loop.getLoc(), 1, 1);
    if (yield.getNumOperands() == 1)
      proceed = arith::XOrIOp::create(rewriter, loop.getLoc(),
                                      yield.getResults()[0], proceed);
    scf::ConditionOp::create(rewriter, loop.getLoc(), proceed, ValueRange{});
    rewriter.eraseOp(loop);
  }

  SmallVector<CallOp> calls;
  body.walk([&](CallOp call) { calls.push_back(call); });
  for (CallOp call : calls) {
    SmallVector<Value> args(call.getArgs());
    args.push_back(arena);
    rewriter.setInsertionPoint(call);
    rewriter.replaceOpWithNewOp<func::CallOp>(call, call.getSchedule(),
                                              TypeRange{}, args);
  }
  WorldAccess world(rewriter, layout, arena);
  lowerResourceAccesses(rewriter, body, world);

  rewriter.setInsertionPoint(body);
  auto func = func::FuncOp::create(
      rewriter, loc, "main",
      rewriter.getFunctionType({}, {rewriter.getI32Type()}));
  rewriter.setInsertionPointToStart(func.addEntryBlock());
  Value created = memref::AllocOp::create(
      rewriter, loc, arenaType,
      rewriter.getI64IntegerAttr(WorldLayout::kArenaAlignment));
  // Counts, resources and entity counters start at zero; columns stay
  // untouched, so capacity costs address space, not memory.
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value end = arith::ConstantIndexOp::create(rewriter, loc, layout.headerBytes);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value zeroByte = arith::ConstantIntOp::create(rewriter, loc, 0, 8);
  scf::ForOp::create(rewriter, loc, zero, end, one, ValueRange{},
                     [&](OpBuilder &builder, Location loc, Value index,
                         ValueRange) {
                       memref::StoreOp::create(builder, loc, zeroByte, created,
                                               ValueRange{index});
                       scf::YieldOp::create(builder, loc);
                     });
  // And the columns in which zero says "none", which must say so from
  // the start: the memory need not be zero.
  for (auto [offset, bytes] : layout.zeroed)
    scf::ForOp::create(
        rewriter, loc, arith::ConstantIndexOp::create(rewriter, loc, offset),
        arith::ConstantIndexOp::create(rewriter, loc, offset + bytes), one,
        ValueRange{},
        [&](OpBuilder &builder, Location loc, Value index, ValueRange) {
          memref::StoreOp::create(builder, loc, zeroByte, created,
                                  ValueRange{index});
          scf::YieldOp::create(builder, loc);
        });
  func::CallOp::create(rewriter, loc, body.getSymName(), TypeRange{},
                       ValueRange{created});
  memref::DeallocOp::create(rewriter, loc, created);
  Value status = arith::ConstantIntOp::create(rewriter, loc, 0, 32);
  func::ReturnOp::create(rewriter, loc, ValueRange{status});
  return success();
}

/// Run the body of a schedule's function `func` only if the schedule's
/// `condition` holds: everything but the return moves into an `scf.if`,
/// except the views of the world's columns at the start of the function,
/// which the condition may share.
static void guardSchedule(IRRewriter &rewriter, func::FuncOp func,
                          Region &condition) {
  if (condition.empty())
    return;
  Block &entry = func.getBody().front();
  Operation *terminator = entry.getTerminator();
  SmallVector<Operation *> body;
  Operation *firstMoved = nullptr;
  for (Operation &op : entry.without_terminator()) {
    if (!firstMoved && op.getNumRegions() == 0 && isPure(&op))
      continue;
    firstMoved = firstMoved ? firstMoved : &op;
    body.push_back(&op);
  }
  if (firstMoved)
    rewriter.setInsertionPoint(firstMoved);
  else
    rewriter.setInsertionPoint(terminator);
  IRMapping mapping;
  for (auto [from, to] : llvm::zip(condition.front().getArguments(),
                                   entry.getArguments()))
    mapping.map(from, to);
  for (Operation &op : condition.front().without_terminator())
    rewriter.clone(op, mapping);
  auto yield = cast<YieldOp>(condition.front().getTerminator());
  auto branch = scf::IfOp::create(rewriter, terminator->getLoc(),
                                  mapping.lookupOrDefault(
                                      yield.getResults()[0]));
  for (Operation *op : body)
    op->moveBefore(branch.thenBlock()->getTerminator());
}

/// Replace a stage by its calls, run one after another, or by an OpenMP
/// parallel region with one section per call.
static void lowerStage(IRRewriter &rewriter, StageOp stage, bool parallel) {
  SmallVector<Operation *> calls;
  for (Operation &op : stage.getBody().front().without_terminator())
    calls.push_back(&op);

  if (!parallel || calls.size() < 2) {
    for (Operation *call : calls)
      call->moveBefore(stage);
    rewriter.eraseOp(stage);
    return;
  }

  Location loc = stage.getLoc();
  rewriter.setInsertionPoint(stage);
  auto parallelOp = omp::ParallelOp::create(rewriter, loc);
  rewriter.createBlock(&parallelOp.getRegion());
  // The region's end is a barrier already; the sections need none of their
  // own (see --ent-omp-nowait).
  omp::SectionsOperands operands;
  operands.nowait = rewriter.getUnitAttr();
  auto sections = omp::SectionsOp::create(rewriter, loc, operands);
  omp::TerminatorOp::create(rewriter, loc);

  rewriter.createBlock(&sections.getRegion());
  for (Operation *call : calls) {
    auto section = omp::SectionOp::create(rewriter, loc);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.createBlock(&section.getRegion());
    call->moveBefore(omp::TerminatorOp::create(rewriter, loc));
  }
  omp::TerminatorOp::create(rewriter, loc);
  rewriter.eraseOp(stage);
}

namespace {
/// Rewrites an op without regions whose operands or results have types the
/// converter changes into the same op on the converted types. This covers
/// ops that merely pass ids along, such as arith.select; ops with regions
/// (functions, scf) have dedicated patterns.
struct ConvertOpTypes : public ConversionPattern {
  ConvertOpTypes(const TypeConverter &converter, MLIRContext *context)
      : ConversionPattern(converter, MatchAnyOpTypeTag(), /*benefit=*/1,
                          context) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    if (op->getNumRegions() != 0 || isa<UnrealizedConversionCastOp>(op))
      return failure();
    SmallVector<Type> resultTypes;
    if (failed(getTypeConverter()->convertTypes(op->getResultTypes(),
                                                resultTypes)))
      return failure();
    OperationState state(op->getLoc(), op->getName().getStringRef(),
                         operands, resultTypes, op->getAttrs(),
                         op->getSuccessors());
    Operation *converted = rewriter.create(state);
    rewriter.replaceOp(op, converted->getResults());
    return success();
  }
};
} // namespace

/// Whether `op` causes the event `trigger` reacts to, for some entity:
/// writes the field (changed), adds the component (added, and changed,
/// since adding sets its fields) or removes it (removed).
static bool causes(Operation *op, const Trigger &trigger) {
  auto sameField = [&](StringRef field) {
    return trigger.field.getValue().empty() ||
           trigger.field.getValue() == field;
  };
  switch (trigger.kind) {
  case Trigger::Changed:
    if (auto set = dyn_cast<SetOp>(op))
      return cast<RefType>(set.getRef().getType()).getComponent() ==
                 trigger.component &&
             sameField(set.getField());
    if (auto apply = dyn_cast<ApplyOp>(op))
      return apply.getComponentAttr() == trigger.component &&
             sameField(apply.getField());
    if (auto combine = dyn_cast<CombineOp>(op))
      return combine.getRef().getType().getComponent() == trigger.component &&
             sameField(combine.getField());
    [[fallthrough]];
  case Trigger::Added:
    if (auto add = dyn_cast<AddOp>(op))
      return add.getComponentAttr() == trigger.component;
    return false;
  case Trigger::Removed:
    if (auto remove = dyn_cast<RemoveOp>(op))
      return remove.getComponentAttr() == trigger.component;
    return false;
  case Trigger::Connected:
    if (auto connect = dyn_cast<ConnectOp>(op))
      return connect.getRelationAttr() == trigger.component;
    return false;
  }
  llvm_unreachable("unknown trigger kind");
}

static std::string describe(const Trigger &trigger) {
  std::string text = trigger.kind == Trigger::Added     ? "added @"
                     : trigger.kind == Trigger::Removed ? "removed @"
                                                        : "changed @";
  text += trigger.component.getValue();
  if (trigger.field && !trigger.field.getValue().empty())
    text += " \"" + trigger.field.getValue().str() + "\"";
  return text;
}

/// Warn about reactive queries that can never fire for a trigger, or only
/// when entities are spawned, and about queries reacting to events they
/// cause themselves: each run collects the entities it changed for the
/// next one, which is most likely a system that should run every frame,
/// be split, or react to a marker component instead.
static void warnAboutReactiveQueries(ModuleOp module) {
  SmallVector<Operation *> causers;
  module.walk([&](Operation *op) {
    if (isa<SetOp, ApplyOp, CombineOp, AddOp, RemoveOp>(op))
      causers.push_back(op);
  });
  module.walk([&](QueryOp query) {
    for (const Trigger &trigger : getTriggers(query)) {
      // (Not a trigger the program wrote.)
      if (trigger.kind == Trigger::Connected || trigger.onTheWay)
        continue;
      // (A module's query reacts to what the program that imports it may
      // do, which is not the module's to know: `module.name` is one.)
      auto system = query->getParentOfType<SystemOp>();
      bool ofModule = system && system.getSymName().contains('.');
      if (trigger.kind != Trigger::Added && !ofModule &&
          llvm::none_of(causers,
                        [&](Operation *op) { return causes(op, trigger); })) {
        if (trigger.kind == Trigger::Removed)
          query.emitWarning("reacts to ")
              << describe(trigger)
              << ", but no system removes it; this trigger never fires";
        else
          query.emitWarning("reacts to ")
              << describe(trigger)
              << ", but no system writes it; this trigger fires only for "
                 "entities spawned with it or gaining it";
      }
      // (An ancestor's event that the query causes where it visits the
      // ancestor is what a trigger up a tree is for; and what it adds
      // into an ancestor from the leaves, that one takes in when the
      // query comes to it.)
      Operation *own = nullptr;
      if (!trigger.via)
        query.getBody().walk([&](Operation *op) {
          if (!own && causes(op, trigger) &&
              !(isa<CombineOp>(op) && query.getCascade()))
            own = op;
        });
      if (own) {
        InFlightDiagnostic diag =
            query.emitWarning("reacts to ")
            << describe(trigger)
            << ", which it causes itself: every run collects the entities "
               "it changed for the next run. Consider a system that runs "
               "every frame, splitting this one, or a marker component";
        diag.attachNote(own->getLoc()) << "causes " << describe(trigger);
      }
    }
  });
}

/// Replace every remaining !ent.entity (function signatures, calls, scf
/// results, ops passing ids along) by the integer the layout stores ids as,
/// and every !ent.text by the integer it is stored as, and fold away the
/// casts the lowering placed at loads and stores.
static LogicalResult convertEntityTypes(ModuleOp module, unsigned idBits) {
  MLIRContext *context = module.getContext();
  auto isEntity = [](Type type) {
    return isa<EntityType, TextType, EnumType>(type);
  };
  bool used = module
                  .walk([&](Operation *op) {
                    for (Region &region : op->getRegions())
                      for (Block &block : region)
                        if (llvm::any_of(block.getArgumentTypes(), isEntity))
                          return WalkResult::interrupt();
                    if (llvm::any_of(op->getOperandTypes(), isEntity) ||
                        llvm::any_of(op->getResultTypes(), isEntity))
                      return WalkResult::interrupt();
                    return WalkResult::advance();
                  })
                  .wasInterrupted();
  if (!used)
    return success();

  TypeConverter converter;
  converter.addConversion([](Type type) { return type; });
  converter.addConversion([&](EntityType) -> Type {
    return IntegerType::get(context, idBits);
  });
  converter.addConversion(
      [](TextType text) -> Type { return text.getStorageType(); });
  converter.addConversion(
      [](EnumType named) -> Type { return named.getStorageType(); });
  auto materialize = [](OpBuilder &builder, Type type, ValueRange inputs,
                        Location loc) -> Value {
    return UnrealizedConversionCastOp::create(builder, loc, type, inputs)
        .getResult(0);
  };
  converter.addSourceMaterialization(materialize);
  converter.addTargetMaterialization(materialize);

  ConversionTarget target(*context);
  target.addLegalOp<UnrealizedConversionCastOp>();
  target.markUnknownOpDynamicallyLegal([&](Operation *op) {
    if (auto func = dyn_cast<func::FuncOp>(op))
      return converter.isSignatureLegal(func.getFunctionType()) &&
             converter.isLegal(&func.getBody());
    return converter.isLegal(op);
  });
  RewritePatternSet patterns(context);
  populateFunctionOpInterfaceTypeConversionPattern<func::FuncOp>(patterns,
                                                                 converter);
  populateCallOpTypeConversionPattern(patterns, converter);
  populateReturnOpTypeConversionPattern(patterns, converter);
  scf::populateSCFStructuralTypeConversionsAndLegality(converter, patterns,
                                                       target);
  patterns.add<ConvertOpTypes>(converter, context);
  if (failed(applyPartialConversion(module, target, std::move(patterns))))
    return failure();

  SmallVector<UnrealizedConversionCastOp> casts;
  module.walk([&](UnrealizedConversionCastOp cast) { casts.push_back(cast); });
  reconcileUnrealizedCasts(casts);
  return success();
}

namespace {
struct EntLowerToLoops
    : public mlir::ent::impl::EntLowerToLoopsBase<EntLowerToLoops> {
  using EntLowerToLoopsBase::EntLowerToLoopsBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    IRRewriter rewriter(module.getContext());
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    FailureOr<WorldLayout> layout = WorldLayout::compute(module);
    if (failed(layout))
      return signalPassFailure();
    // Tag every apply with its buffers' index in the layout, which lists
    // them in the same walk order; clones keep the tag.
    unsigned applyIndex = 0;
    module.walk([&](Operation *apply) {
      if (isa<ApplyOp, AccumulateOp>(apply))
        apply->setAttr(WorldLayout::kApplyIndexAttr,
                       rewriter.getI64IntegerAttr(applyIndex++));
    });
    warnAboutReactiveQueries(module);
    // The same for reactive queries and their last ticks.
    unsigned reactiveIndex = 0;
    module.walk([&](QueryOp query) {
      if (!getTriggers(query).empty())
        query->setAttr(WorldLayout::kReactiveIndexAttr,
                       rewriter.getI64IntegerAttr(reactiveIndex++));
    });
    // Applies whose field nothing else in their query touches.
    module.walk([&](ApplyOp apply) {
      auto query = apply->getParentOfType<QueryOp>();
      FlatSymbolRefAttr component = apply.getComponentAttr();
      StringAttr field = apply.getFieldAttr();
      auto refTo = [&](Value ref) {
        return cast<RefType>(ref.getType()).getComponent() == component;
      };
      WalkResult seen = query.walk([&](Operation *op) {
        bool touches = false;
        if (auto other = dyn_cast<ApplyOp>(op))
          touches = other != apply && other.getComponentAttr() == component &&
                    other.getFieldAttr() == field;
        else if (auto get = dyn_cast<GetOp>(op))
          touches = refTo(get.getRef()) && get.getFieldAttr() == field;
        else if (auto set = dyn_cast<SetOp>(op))
          touches = refTo(set.getRef()) && set.getFieldAttr() == field;
        else if (auto lookup = dyn_cast<LookupOp>(op))
          touches = lookup.getComponentAttr() == component &&
                    lookup.getFieldAttr() == field;
        else if (isa<AddOp, RemoveOp>(op))
          touches = op->getAttr("component") == component;
        return touches ? WalkResult::interrupt() : WalkResult::advance();
      });
      if (!seen.wasInterrupted())
        apply->setAttr(kUnobservedAttr, rewriter.getUnitAttr());
    });
    module.walk([&](AccumulateOp accumulate) {
      auto query = accumulate->getParentOfType<QueryOp>();
      auto resource = accumulate.getResourceAttr();
      StringAttr field = accumulate.getFieldAttr();
      WalkResult seen = query.walk([&](Operation *op) {
        if (auto read = dyn_cast<ReadOp>(op))
          if (read.getResourceAttr() == resource && read.getFieldAttr() == field)
            return WalkResult::interrupt();
        if (auto other = dyn_cast<AccumulateOp>(op))
          if (other != accumulate && other.getResourceAttr() == resource &&
              other.getFieldAttr() == field)
            return WalkResult::interrupt();
        return WalkResult::advance();
      });
      if (!seen.wasInterrupted())
        accumulate->setAttr(kUnobservedAttr, rewriter.getUnitAttr());
    });
    // A cascading query combines what it sends as it visits: its order is
    // the order of its depths' ends, and it reads none of those fields.
    module.walk([&](Operation *op) {
      if (!isa<ApplyOp, AccumulateOp>(op))
        return;
      if (op->getParentOfType<QueryOp>().getCascade())
        op->setAttr(kUnobservedAttr, rewriter.getUnitAttr());
    });
    // Lookups and applies through a trusted end of an edge.
    module.walk([&](Operation *op) {
      if (!isa<LookupOp, ApplyOp>(op))
        return;
      auto arg = dyn_cast<BlockArgument>(op->getOperand(0));
      auto edges =
          arg ? dyn_cast<EdgesOp>(arg.getOwner()->getParentOp()) : EdgesOp();
      if (!edges || arg.getArgNumber() != 1)
        return;
      auto relation = SymbolTable::lookupNearestSymbolFrom<RelationOp>(
          edges, edges.getRelationAttr());
      FlatSymbolRefAttr trusted =
          getTrustedEndpoint(relation, /*target=*/edges.isOut());
      if (trusted && trusted == op->getAttr("component"))
        op->setAttr(kTrustedAttr, rewriter.getUnitAttr());
    });
    // And for connects inside queries and their buffers.
    unsigned connectIndex = 0;
    module.walk([&](ConnectOp connect) {
      if (connect->getParentOfType<QueryOp>())
        connect->setAttr(WorldLayout::kConnectIndexAttr,
                         rewriter.getI64IntegerAttr(connectIndex++));
    });
    MemRefType arenaType = getArenaType(module.getContext(), *layout);
    for (const WorldRelation &relation : layout->relations)
      emitSortFunction(rewriter, module, *layout, relation, arenaType);
    LoopOptions options{parallelEntities, parallelMinEntities,
                        parallelMinEvents, parallelMinLevel, explain,
                        directApplies};
    SymbolTable symbols(module);

    for (ExternOp external : module.getOps<ExternOp>())
      if (failed(declareExtern(rewriter, external, symbols)))
        return signalPassFailure();

    // The functions with a body, which lowering takes out of them.
    SmallPtrSet<Operation *, 8> defined;
    for (FunctionOp function : module.getOps<FunctionOp>()) {
      if (function.isDefined())
        defined.insert(function);
      if (failed(declareFunction(rewriter, function, symbols)))
        return signalPassFailure();
    }

    // Schedules first: fusion reads the systems' bodies before they are
    // lowered themselves.
    for (auto schedule :
         llvm::make_early_inc_range(module.getOps<ScheduleOp>())) {
      // The function replaces the schedule; keep its condition apart.
      Region condition;
      condition.takeBody(schedule.getCondition());
      auto [func, arena] = convertToFunc(rewriter, schedule,
                                         schedule.getSymName(),
                                         schedule.getBody(), arenaType);
      func->setAttr("llvm.emit_c_interface", rewriter.getUnitAttr());
      WorldAccess world(rewriter, *layout, arena);
      if (fuseSystems)
        fuseSchedule(rewriter, func, symbols, *layout, world, options);
      SmallVector<RunOp> runs;
      func.walk([&](RunOp run) { runs.push_back(run); });
      for (RunOp run : runs)
        lowerRun(rewriter, run, arena, symbols, *layout);
      SmallVector<StageOp> stages(func.getOps<StageOp>());
      for (StageOp stage : stages)
        lowerStage(rewriter, stage, parallelStages);
      // Edges the host connected since the last frame are sorted first.
      rewriter.setInsertionPointToStart(&func.getBody().front());
      for (const WorldRelation &relation : layout->relations)
        callSort(rewriter, func.getLoc(), relation, arena);
      guardSchedule(rewriter, func, condition);
      lowerResourceAccesses(rewriter, func, world);
    }

    for (auto system : llvm::make_early_inc_range(module.getOps<SystemOp>())) {
      auto [func, arena] = convertToFunc(
          rewriter, system, system.getSymName(), system.getBody(), arenaType);
      func.setPrivate();
      WorldAccess world(rewriter, *layout, arena);
      SmallVector<QueryOp> queries;
      func.walk([&](QueryOp query) { queries.push_back(query); });
      // A relation the system connects outside its queries, and an
      // archetype sorted by a tree that it spawns into there, are sorted
      // before its next query and when it ends, rather than after every
      // connect or spawn: nothing in between can see the edges.
      llvm::SetVector<Attribute> unsorted;
      func.walk([&](ConnectOp connect) {
        if (!connect->getParentOfType<QueryOp>())
          unsorted.insert(connect.getRelationAttr().getAttr());
      });
      func.walk([&](SpawnOp spawn) {
        if (spawn->getParentOfType<QueryOp>())
          return;
        for (const WorldArchetype &archetype : layout->archetypes)
          if (archetype.isSorted() &&
              ArchetypeOp(archetype.op).getSymNameAttr() ==
                  spawn.getArchetypeAttr().getAttr())
            unsorted.insert(archetype.sortedBy);
      });
      if (!unsorted.empty()) {
        SmallVector<Operation *> before(queries.begin(), queries.end());
        func.walk([&](func::ReturnOp op) { before.push_back(op); });
        for (Operation *op : before) {
          rewriter.setInsertionPoint(op);
          for (Attribute relation : unsorted)
            callSort(rewriter, op->getLoc(),
                     layout->getRelation(cast<StringAttr>(relation)), arena);
        }
      }
      for (QueryOp query : queries)
        lowerQuery(rewriter, query, *layout, world, options);
      lowerSpawns(rewriter, func, *layout, world);
      lowerConnects(rewriter, func, *layout, world);
      // Innermost first, so an outer `if` sees merged inner ones.
      SmallVector<scf::IfOp> branches;
      func.walk<WalkOrder::PostOrder>(
          [&](scf::IfOp branch) { branches.push_back(branch); });
      for (scf::IfOp branch : branches)
        mergeBranchStores(rewriter, branch);
      lowerLookups(rewriter, func, *layout, world);
      lowerResourceAccesses(rewriter, func, world);
      shareParts(rewriter, func);
    }

    SmallVector<InvokeOp> invokes;
    module.walk([&](InvokeOp invoke) { invokes.push_back(invoke); });
    for (InvokeOp invoke : invokes) {
      auto function = symbols.lookup<FunctionOp>(invoke.getCallee());
      lowerInvoke(rewriter, invoke, function, defined.contains(function));
    }

    for (auto main : llvm::make_early_inc_range(module.getOps<MainOp>()))
      if (failed(lowerMain(rewriter, main, module, *layout, arenaType)))
        return signalPassFailure();

    for (Operation &op : llvm::make_early_inc_range(module.getOps()))
      if (isa<ComponentOp, ResourceOp, ArchetypeOp, RelationOp, ExternOp,
              FunctionOp, EnumOp>(op))
        rewriter.eraseOp(&op);

    if (failed(convertEntityTypes(module, layout->entities.idBits)))
      return signalPassFailure();
  }
};
} // namespace
