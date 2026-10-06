#include "Ent/Access.h"
#include "Ent/EntOps.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"
#include "Ent/World.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/Transforms/Patterns.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
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
    return view(relation.orderOffset, relation.capacity, idType());
  }
  Value treeOrderParents(const WorldRelation &relation) {
    return view(relation.orderParentOffset, relation.capacity, idType());
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
    Value &value = columns[{~1u, offset}];
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
};

/// How entity loops are emitted.
struct LoopOptions {
  bool parallelEntities;
  int64_t parallelMinEntities;
  /// Pending log entries from which a reactive query walks its event logs'
  /// segments in parallel.
  int64_t parallelMinEvents;
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
/// `tick` already and `when` holds (null for always). Once the segment is
/// full for every reader, appending is wasted work: the first event that
/// finds it full instead moves its count one past (every reader scans),
/// and later events find nothing to do. With `atomic`, iterations of a
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
  Value count =
      memref::LoadOp::create(rewriter, loc, counts, ValueRange{countAt});
  Value slowest =
      memref::LoadOp::create(rewriter, loc, counts, ValueRange{slowestAt});
  Value pending = arith::SubIOp::create(rewriter, loc, count, slowest);
  Value room = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::slt,
                                     pending, i64(log.segmentCapacity));
  Value full = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq,
                                     pending, i64(log.segmentCapacity));
  OpBuilder::InsertionGuard guard(rewriter);
  auto append = scf::IfOp::create(
      rewriter, loc, arith::AndIOp::create(rewriter, loc, event, room));
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

  rewriter.setInsertionPointAfter(append);
  auto overflow = scf::IfOp::create(
      rewriter, loc, arith::AndIOp::create(rewriter, loc, event, full));
  rewriter.setInsertionPointToStart(overflow.thenBlock());
  Value past = arith::AddIOp::create(rewriter, loc, slowest,
                                     i64(log.segmentCapacity + 1));
  if (atomic)
    memref::AtomicRMWOp::create(rewriter, loc, arith::AtomicRMWKind::maxs,
                                past, counts, ValueRange{countAt});
  else
    memref::StoreOp::create(rewriter, loc, past, counts, ValueRange{countAt});
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

/// Replace the get/set/add/remove/despawn/entity ops nested in `roots` by
/// loads and stores at `entity` in the columns of `archetype`. With a
/// `mask`, every store keeps the old value where the mask is false: the
/// body ran for an entity it does not apply to.
static void lowerAccesses(IRRewriter &rewriter, ArrayRef<Operation *> roots,
                          const WorldArchetype &archetype, WorldAccess &world,
                          const WorldLayout &layout, Value entity,
                          Value rows, Value mask, Value tick, bool parallel,
                          bool directApplies) {
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
  auto stamp = [&](Location loc, Trigger::Kind kind, StringAttr component,
                   StringAttr field = StringAttr()) {
    for (const WorldColumn *column :
         stampsFor(archetype, kind, component, field)) {
      assert(tick && "an event is stamped without a tick");
      Value stamps = world.stamps(archetype, *column);
      const WorldLog *log = layout.findLog(*column->stamp);
      Value old = log ? memref::LoadOp::create(rewriter, loc, stamps,
                                               ValueRange{entity})
                            .getResult()
                      : Value();
      store(loc, tick, stamps);
      if (log)
        appendToLog(rewriter, loc, world, *log,
                    segmentOf(rewriter, loc, world, *log, entity, rows),
                    world.entityId(loc, archetype, entity), tick, old, mask,
                    parallel);
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
      store(loc, set.getValue(),
            column(component.getAttr(), set.getFieldAttr()));
      stamp(loc, Trigger::Changed, component.getAttr(), set.getFieldAttr());
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
                             Value parent = Value());
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
                          KnownParent parent = {}) {
  Location loc = query.getLoc();
  ArchetypeOp archetypeOp = archetype.op;
  Value mask;
  // A reactive query applies only to the entities with an event since it
  // last started (`seen`): a stamp newer than that, for any trigger.
  if (seen) {
    for (const Trigger &trigger : getTriggers(query)) {
      const WorldColumn *column = archetype.findStamp(getStamp(trigger));
      if (!column)
        continue;
      Value stamped = memref::LoadOp::create(
          rewriter, loc, world.stamps(archetype, *column), ValueRange{entity});
      Value newer = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::sgt, stamped, seen);
      mask = mask ? arith::OrIOp::create(rewriter, loc, mask, newer) : newer;
    }
    assert(mask && "a reactive query is lowered for an archetype where no "
                   "trigger can fire");
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
    if (known.row &&
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
                                world.entityId(loc, archetype, entity));
      }
    } else {
      if (known.row)
        known.id = memref::LoadOp::create(rewriter, loc,
                                          world.ids(*known.archetype),
                                          ValueRange{known.row});
      ancestor = emitAncestor(rewriter, loc, layout, world, relation,
                              refType.getComponent(),
                              world.entityId(loc, archetype, entity),
                              known.id);
    }
    // Null: every entity the caller visits has one.
    if (ancestor.found)
      require(ancestor.found);
    ancestors.push_back({arg, ancestor});
  }

  OpBuilder::InsertionGuard guard(rewriter);
  // Without an ancestor there is nothing to read: such a body never runs
  // masked.
  bool guarded = mask && (!canRunForAbsentEntities(query) ||
                          isStructuralFor(query, archetypeOp) ||
                          !ancestors.empty());
  assert((mask || ancestors.empty() ||
          llvm::all_of(ancestors, [](auto &entry) {
            return !entry.second.found;
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
  // Reading through a ref to an ancestor is a lookup of the ancestor,
  // which is known to be found.
  for (auto &[arg, ancestor] : ancestors) {
    SmallVector<GetOp> gets;
    for (Operation *root : roots)
      root->walk([&, arg = arg](GetOp get) {
        if (get.getRef() == arg)
          gets.push_back(get);
      });
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
      if (ancestor.trusted)
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
                tick, parallel, directApplies);
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
    } else if (!action) {
      // Only despawns are ever listed for this archetype.
      world.freeEntity(loc, id);
    } else {
      auto despawn = scf::IfOp::create(rewriter, loc, is(0));
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(despawn.thenBlock());
      world.freeEntity(loc, id);
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
    for (auto [log, stamped] : spawned)
      appendToLog(rewriter, loc, world, *log,
                  segmentOf(rewriter, loc, world, *log, row, Value()), id,
                  stamped, Value(), Value(), /*atomic=*/false);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    world.setCount(loc, *archetype,
                   arith::AddIOp::create(rewriter, loc, row, one));
    rewriter.replaceOp(spawn, world.fromStorage(loc, id, spawn.getType()));
  }
}

/// Bounds that ids are checked against, loaded ahead by a caller that
/// knows they cannot change (see emitLocate).
namespace {
struct LocateBounds {
  /// Entity counts by archetype index (Rows ids).
  SmallVector<Value> counts;
  /// Slots in use, as an index (slot ids).
  Value slotsInUse;
};
} // namespace

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
           const LocateBounds &bounds = {}, bool trusted = false) {
  const EntityScheme &scheme = layout.entities;
  if (trusted) {
    const WorldArchetype *only = nullptr;
    unsigned candidates = 0;
    for (const WorldArchetype &archetype : layout.archetypes)
      if (candidate(archetype)) {
        only = &archetype;
        ++candidates;
      }
    if (candidates == 1) {
      Value row = scheme.kind == EntityScheme::Rows
                      ? world.unpackRows(loc, id).second
                      : world.getLocation(loc, world.idSlot(loc, id)).second;
      return found(*only, row, Value());
    }
    if (candidates > 1) {
      auto [where, row] =
          scheme.kind == EntityScheme::Rows
              ? world.unpackRows(loc, id)
              : world.getLocation(loc, world.idSlot(loc, id));
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
static void lowerLookups(IRRewriter &rewriter, func::FuncOp func,
                         const WorldLayout &layout, WorldAccess &world) {
  SmallVector<LookupOp> lookups;
  func.walk([&](LookupOp lookup) { lookups.push_back(lookup); });
  for (LookupOp lookup : lookups) {
    Location loc = lookup.getLoc();
    rewriter.setInsertionPoint(lookup);
    Type type = world.storageType(lookup.getValue().getType());
    SmallVector<Type, 2> resultTypes{type, rewriter.getI1Type()};
    FlatSymbolRefAttr component = lookup.getComponentAttr();
    auto holds = [&](const WorldArchetype &archetype) {
      return ArchetypeOp(archetype.op).contains(component);
    };
    SmallVector<Value> results = emitLocate(
        rewriter, loc, layout, world, world.toStorage(loc, lookup.getEntity()),
        holds, component, resultTypes,
        [&](const WorldArchetype &archetype, Value row,
            Value present) -> SmallVector<Value> {
          Value value = memref::LoadOp::create(
              rewriter, loc,
              world.column(archetype, component.getAttr(),
                           lookup.getFieldAttr()),
              ValueRange{row});
          if (!present)
            present = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
          return {value, present};
        },
        [&]() -> SmallVector<Value> {
          Value zero =
              isa<FloatType>(type)
                  ? arith::ConstantOp::create(rewriter, loc,
                                              rewriter.getFloatAttr(type, 0.0))
                        .getResult()
                  : arith::ConstantOp::create(rewriter, loc,
                                              rewriter.getIntegerAttr(type, 0))
                        .getResult();
          Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
          return {zero, no};
        },
        LocateBounds(), lookup->hasAttr(kTrustedAttr));
    rewriter.replaceOp(lookup,
                       {world.fromStorage(loc, results[0],
                                          lookup.getValue().getType()),
                        results[1]});
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
  memref::StoreOp::create(rewriter, loc,
                          combine(rewriter, loc, rule, old, value), field,
                          ValueRange{row});
  for (const WorldColumn *column :
       stampsFor(target, Trigger::Changed, component, fieldName)) {
    Value stamps = world.stamps(target, *column);
    const WorldLog *log = layout.findLog(*column->stamp);
    Value before = log ? memref::LoadOp::create(rewriter, loc, stamps,
                                                ValueRange{row})
                             .getResult()
                       : Value();
    memref::StoreOp::create(rewriter, loc, tick, stamps, ValueRange{row});
    if (log)
      appendToLog(rewriter, loc, world, *log,
                  segmentOf(rewriter, loc, world, *log, row, Value()),
                  id ? id : world.entityId(loc, target, row), tick, before,
                  Value(), /*atomic=*/false);
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
/// then null where the parent is the ancestor without a doubt.
static Ancestor emitAncestor(IRRewriter &rewriter, Location loc,
                             const WorldLayout &layout, WorldAccess &world,
                             const WorldRelation &relation,
                             FlatSymbolRefAttr component, Value id,
                             Value parent) {
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
  if (trusted)
    return {has, parent, /*trusted=*/true};
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
  void linkUnder(Value slot, Value parent) {
    Value last = load(lastChild(), parent);
    Value self = link(arith::AddIOp::create(rewriter, loc, slot, one));
    store(last, previousSibling(), slot);
    store(noLink(), nextSibling(), slot);
    branch(
        isNone(last), [&] { store(self, firstChild(), parent); },
        [&] {
          store(self, nextSibling(),
                arith::SubIOp::create(rewriter, loc, world.toIndex(loc, last),
                                      one));
        });
    store(self, lastChild(), parent);
    store(arith::AddIOp::create(
              rewriter, loc, load(childCount(), parent),
              arith::ConstantIntOp::create(rewriter, loc, 1,
                                           relation.offsetBits)),
          childCount(), parent);
  }
  /// The slot `slot` leaves the children of the key its target has.
  void unlink(Value slot) {
    Value parent = world.entityKey(loc, load(targets(), slot));
    Value previous = load(previousSibling(), slot);
    Value next = load(nextSibling(), slot);
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
    store(link(arith::AddIOp::create(rewriter, loc, at, one)), position(),
          key);
    store(arith::AddIOp::create(rewriter, loc, length, i64(1)),
          world.treeOrderCount(relation), zero);
  }
  void markUnclean() { store(i64(0), world.edgesClean(relation), zero); }
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
  tree.branch(isOwn, [&] { tree.unlink(slot); });
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
              tree.store(target, world.treeOrderParents(relation),
                         arith::SubIOp::create(rewriter, loc,
                                               world.toIndex(loc, ownAt),
                                               one));
            },
            [&] { tree.markUnclean(); });
      },
      [&] {
        Value childless = tree.isNone(tree.load(tree.childCount(), slot));
        Value parentListed = arith::OrIOp::create(
            rewriter, loc, tree.negate(parentHas),
            tree.negate(tree.isNone(parentAt)));
        Value room = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::slt, tree.listed(),
            tree.i64(relation.capacity));
        tree.branch(
            tree.both(childless, tree.both(parentListed, room)),
            [&] { tree.list(source, target, slot); },
            [&] { tree.markUnclean(); });
      });
}

/// Emit the function that, if a linked tree is unclean, builds it again
/// from its slots: the edges of dead entities, to dead entities and
/// disconnected ones go; every entity's children are linked in the order
/// of their keys; and the entities with a parent are listed breadth first,
/// the children of those without one by their keys, then the children of
/// each entity listed. An entity on a cycle is never reached, which stops
/// the program.
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

  auto hasEdge = [&](Value key) {
    return tree.negate(
        tree.same(tree.load(tree.sources(), key), tree.noOwner()));
  };
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
  // Children, by their keys.
  tree.forEach(zero, keys, [&](Value key) {
    tree.branch(hasEdge(key), [&] {
      tree.linkUnder(key,
                     world.entityKey(loc, tree.load(tree.targets(), key)));
    });
  });
  // The list: the children of the entities without a parent, then the
  // children of each entity listed.
  tree.store(tree.i64(0), world.treeOrderCount(relation), zero);
  tree.forEach(zero, keys, [&](Value key) {
    tree.branch(hasEdge(key), [&] {
      Value parent = tree.load(tree.targets(), key);
      tree.branch(tree.negate(hasEdge(world.entityKey(loc, parent))), [&] {
        tree.list(tree.sourceOf(key), parent, key);
      });
    });
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
        Value id = tree.load(world.treeOrder(relation), state[0]);
        Value key = world.entityKey(loc, id);
        Value number = world.toIndex(loc, tree.load(tree.childCount(), key));
        Value first = world.toIndex(loc, tree.load(tree.firstChild(), key));
        auto children = scf::ForOp::create(rewriter, loc, zero, number, one,
                                           ValueRange{first});
        {
          OpBuilder::InsertionGuard inner(rewriter);
          rewriter.setInsertionPointToStart(children.getBody());
          Value child = arith::SubIOp::create(
              rewriter, loc, children.getRegionIterArg(0), one);
          tree.list(tree.sourceOf(child), id, child);
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{world.toIndex(
                  loc, tree.load(tree.nextSibling(), child))});
        }
        scf::YieldOp::create(
            rewriter, loc,
            ValueRange{arith::AddIOp::create(rewriter, loc, state[0], one)});
      });
  cf::AssertOp::create(
      rewriter, loc,
      tree.same(tree.listed(), tree.load(world.edgeCount(relation), zero)),
      rewriter.getStringAttr("@" + relationOp.getSymName() +
                             " is a tree, but an entity is its own "
                             "ancestor"));
  tree.store(tree.i64(1), world.edgesClean(relation), zero);
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
    auto list = [&](Value id, Value parent) {
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
    forEach(zero, kept, [&](Value k) {
      Value parent =
          memref::LoadOp::create(rewriter, loc, targets, ValueRange{k});
      auto [begin, end] = emitEdgeRange(rewriter, loc, world, relation,
                                        /*in=*/false,
                                        world.entityKey(loc, parent));
      auto ifRoot = scf::IfOp::create(
          rewriter, loc,
          arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::eq, begin,
                                end));
      OpBuilder::InsertionGuard inner(rewriter);
      rewriter.setInsertionPointToStart(ifRoot.thenBlock());
      list(memref::LoadOp::create(rewriter, loc, sources, ValueRange{k}),
           parent);
    });
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
          forEach(begin, end, [&](Value position) {
            Value edge = world.toIndex(
                loc, memref::LoadOp::create(rewriter, loc,
                                            world.indexEdges(relation),
                                            ValueRange{position}));
            list(memref::LoadOp::create(rewriter, loc, sources,
                                        ValueRange{edge}),
                 id);
          });
          scf::YieldOp::create(
              rewriter, loc,
              ValueRange{arith::AddIOp::create(rewriter, loc, state[0], one)});
        });
    Value listed =
        memref::LoadOp::create(rewriter, loc, length, ValueRange{zero});
    cf::AssertOp::create(
        rewriter, loc,
        arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq, listed,
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
      Value entries = world.toIndex(loc, listed);
      auto locationOf = [&](Value id) {
        return world.getLocation(loc, world.idSlot(loc, id));
      };
      if (alone) {
        forEach(zero, entries, [&](Value k) {
          Value id =
              memref::LoadOp::create(rewriter, loc, order, ValueRange{k});
          memref::StoreOp::create(
              rewriter, loc,
              asRow(arith::AddIOp::create(rewriter, loc, roots[0], k)),
              newRowsOf(*holders[0]), ValueRange{locationOf(id).second});
        });
      } else {
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
      } else {
        forEach(zero, entries, [&](Value k) {
          Value id =
              memref::LoadOp::create(rewriter, loc, order, ValueRange{k});
          Value parent = memref::LoadOp::create(rewriter, loc, orderParents,
                                                ValueRange{k});
          Value packed = memref::LoadOp::create(
              rewriter, loc, world.locations(),
              ValueRange{world.idSlot(loc, parent)});
          auto [where, row] = locationOf(id);
          for (const WorldArchetype *archetype : holders) {
            auto ifHere =
                scf::IfOp::create(rewriter, loc, isIn(where, *archetype));
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(ifHere.thenBlock());
            memref::StoreOp::create(rewriter, loc, packed,
                                    world.parentLocations(*archetype),
                                    ValueRange{row});
          }
        });
      }
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
  for (const Trigger &trigger : getTriggers(query))
    if (!layout.findLog(getStamp(trigger)))
      return "the event log of a trigger has capacity 0";
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
          emitQueryBody(rewriter, query, IRMapping(), archetype, world, layout,
                        row, world.count(loc, archetype), tick, Value(),
                        parallel);
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

/// Replace a cascading query by loops that visit parents before their
/// children: first the entities without a parent in the tree, archetype
/// after archetype, then those with one in the order the relation's sort
/// left (breadth first), each found by its id. Entities of one depth read
/// only what shallower ones wrote (through refs up the tree), so one pass
/// in this order is the query run once per depth. A query with a ref up
/// the tree it cascades along visits no entity without a parent. With
/// `leaves first` the order is the other way: the list from its end, then
/// the entities without a parent. What the query combines into ancestors
/// it combines as it visits, which is the order of the depths' ends.
static void lowerCascade(IRRewriter &rewriter, QueryOp query,
                         const WorldLayout &layout, WorldAccess &world,
                         const LoopOptions &options) {
  Location loc = query.getLoc();
  FlatSymbolRefAttr cascade = query.getCascade();
  const WorldRelation &relation = layout.getRelation(cascade.getAttr());
  rewriter.setInsertionPoint(query);
  Value tick = world.hasStamps() ? world.currentTick(loc) : Value();
  bool matched = false;
  bool visitsRoots =
      llvm::none_of(query.getBody().getArgumentTypes(), [&](Type type) {
        return cast<RefType>(type).getVia() == cascade;
      });
  LoopOptions sequential = options;
  sequential.parallelEntities = false;
  bool leavesFirst = query.isLeavesFirst();
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
  auto emitRoots = [&] {
  for (const WorldArchetype &archetype : layout.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    if (!visitsRoots)
      continue;
    rewriter.setInsertionPoint(query);
    if (&archetype == sorted || llvm::is_contained(levelled, &archetype)) {
      Operation *loops = emitEntityLoops(
          rewriter, loc, archetype, world, sequential, /*entityLocal=*/false,
          [&](Value entity, Value rows, bool) {
            emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                          layout, entity, rows, tick, Value(),
                          /*parallel=*/false, /*directApplies=*/true);
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
          emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                        layout, entity, rows, tick, Value(),
                        /*parallel=*/false, /*directApplies=*/true);
        });
    hoistResourceReads(rewriter, loops, world);
  }
  };
  if (!leavesFirst)
    emitRoots();
  else
    matched = llvm::any_of(layout.archetypes,
                           [&](const WorldArchetype &archetype) {
                             return matches(archetype, query);
                           });
  if (matched && sorted) {
    // Every entity with a parent is in the sorted archetype: its rows
    // after those without one, up or down, each with its parent's row.
    if (matches(*sorted, query)) {
      rewriter.setInsertionPoint(query);
      Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
      Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
      Value rows = world.count(loc, *sorted);
      Value roots = rootCount(*sorted);
      auto loop = scf::ForOp::create(
          rewriter, loc, zero,
          arith::SubIOp::create(rewriter, loc, rows, roots), one);
      rewriter.setInsertionPoint(loop.getBody()->getTerminator());
      Value row = leavesFirst
                      ? arith::SubIOp::create(
                            rewriter, loc,
                            arith::SubIOp::create(rewriter, loc, rows, one),
                            loop.getInductionVar())
                            .getResult()
                      : arith::AddIOp::create(rewriter, loc, roots,
                                              loop.getInductionVar())
                            .getResult();
      KnownParent parent{
          &relation, Value(), sorted,
          world.toIndex(loc, memref::LoadOp::create(rewriter, loc,
                                                    world.parentRows(*sorted),
                                                    ValueRange{row}))};
      emitQueryBody(rewriter, query, IRMapping(), *sorted, world, layout, row,
                    rows, tick, Value(), /*parallel=*/false,
                    /*directApplies=*/true, parent);
      hoistResourceReads(rewriter, loop, world);
    }
    if (leavesFirst)
      emitRoots();
  } else if (matched && !levelled.empty()) {
    // Depth by depth, from the first under the roots down or from the
    // last up; in each, the rows every archetype has of it, each with its
    // parent's location. Children first takes the archetypes and the rows
    // the other way round too, so that it is parents first backwards.
    rewriter.setInsertionPoint(query);
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    Value depths = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc, world.treeDepth(relation),
                                    ValueRange{zero}));
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
        auto rows = scf::ForOp::create(
            rewriter, loc, zero, arith::SubIOp::create(rewriter, loc, to, from),
            one);
        rewriter.setInsertionPoint(rows.getBody()->getTerminator());
        Value row = leavesFirst
                        ? arith::SubIOp::create(
                              rewriter, loc,
                              arith::SubIOp::create(rewriter, loc, to, one),
                              rows.getInductionVar())
                              .getResult()
                        : arith::AddIOp::create(rewriter, loc, from,
                                                rows.getInductionVar())
                              .getResult();
        KnownParent parent{&relation};
        parent.location = memref::LoadOp::create(
            rewriter, loc, world.parentLocations(*archetype), ValueRange{row});
        emitQueryBody(rewriter, query, IRMapping(), *archetype, world, layout,
                      row, world.count(loc, *archetype), tick, Value(),
                      /*parallel=*/false, /*directApplies=*/true, parent);
      }
    }
    hoistResourceReads(rewriter, levels, world);
    if (leavesFirst)
      emitRoots();
  } else if (matched) {
    rewriter.setInsertionPoint(query);
    Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    Value count = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc,
                                    world.treeOrderCount(relation),
                                    ValueRange{zero}));
    auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    Value at = loop.getInductionVar();
    if (leavesFirst)
      at = arith::SubIOp::create(
          rewriter, loc, arith::SubIOp::create(rewriter, loc, count, one), at);
    Value id = memref::LoadOp::create(rewriter, loc, world.treeOrder(relation),
                                      ValueRange{at});
    // The list has each entity's parent next to it.
    KnownParent parent{&relation,
                       memref::LoadOp::create(rewriter, loc,
                                              world.treeOrderParents(relation),
                                              ValueRange{at})};
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
    bool certain = from ? fromTrusted : !layout.entities.hasGenerations();
    for (const WorldArchetype &archetype : layout.archetypes)
      certain &= !isHome(archetype) || matches(archetype, query);
    emitLocate(
        rewriter, loc, layout, world, id,
        [&](const WorldArchetype &archetype) {
          return certain ? isHome(archetype) : matches(archetype, query);
        },
        FlatSymbolRefAttr(), TypeRange{},
        [&](const WorldArchetype &archetype, Value row,
            Value) -> SmallVector<Value> {
          OpBuilder::InsertionGuard inner(rewriter);
          emitQueryBody(rewriter, query, IRMapping(), archetype, world, layout,
                        row, world.count(loc, archetype), tick, Value(),
                        /*parallel=*/false, /*directApplies=*/true, parent);
          return {};
        },
        []() -> SmallVector<Value> { return {}; }, LocateBounds(), certain);
    hoistResourceReads(rewriter, loop, world);
    if (leavesFirst)
      emitRoots();
  } else {
    query.emitWarning("matches no archetype; the query is removed");
  }
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
      Value lost = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::sgt,
          arith::SubIOp::create(rewriter, loc, end, from),
          arith::ConstantIntOp::create(rewriter, loc, log.segmentCapacity,
                                       64));
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
          return archetype.findStamp(getStamp(trigger));
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
    Operation *loops = emitEntityLoops(
        rewriter, loc, archetype, world, options, entityLocal,
        [&](Value entity, Value rows, bool parallel) {
          emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                        layout, entity, rows, tick, seen, parallel,
                        sequentialOnly);
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
    // The query has read every segment to where it ended when it started;
    // the slowest reader of each segment bounds how far writers append.
    rewriter.setInsertionPoint(query);
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
    }
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
    applyPending(rewriter, loc, layout, *archetype, world, tick);
  }
  // Then the changed relations are sorted, which also drops edges to the
  // entities just despawned.
  for (Attribute relation : changedRelations)
    callSort(rewriter, loc, layout.getRelation(cast<StringAttr>(relation)),
             world.getArena());

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
    if (isa<SetOp, ApplyOp, AddOp, RemoveOp>(op))
      causers.push_back(op);
  });
  module.walk([&](QueryOp query) {
    for (const Trigger &trigger : getTriggers(query)) {
      if (trigger.kind != Trigger::Added &&
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
      Operation *own = nullptr;
      query.getBody().walk([&](Operation *op) {
        if (!own && causes(op, trigger))
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
                        parallelMinEvents, explain, directApplies};
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
