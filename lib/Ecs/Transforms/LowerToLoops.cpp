#include "Ecs/Access.h"
#include "Ecs/EcsOps.h"
#include "Ecs/Passes.h"
#include "Ecs/Structure.h"
#include "Ecs/World.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/SCF/Transforms/Patterns.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ecs {
#define GEN_PASS_DEF_ECSLOWERTOLOOPS
#include "Ecs/Passes.h.inc"
} // namespace mlir::ecs

using namespace mlir;
using namespace mlir::ecs;

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

  /// The type a value of `type` is stored as: entity ids become integers
  /// of the width the layout chose; everything else is stored as is.
  Type storageType(Type type) {
    if (isa<EntityType>(type))
      return rewriter.getIntegerType(layout.entities.idBits);
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
    memref::StoreOp::create(
        rewriter, loc,
        arith::AddIOp::create(
            rewriter, loc, generation,
            arith::ConstantIntOp::create(rewriter, loc, 1,
                                         scheme.generationStorageBits)),
        generations(), ValueRange{slot});
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
  ArchetypeOp archetypeOp = archetype.op;
  return llvm::all_of(query.getBody().getArgumentTypes(), [&](Type type) {
    return archetypeOp.contains(cast<RefType>(type).getComponent());
  });
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
    if (isa<GetOp, SetOp, ReadOp, AddOp, RemoveOp, EntityOp, LookupOp,
            YieldOp>(op) ||
        !hasOwnEffects(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !result.wasInterrupted();
}

/// Emit the loop over the entities of `archetype` at the insertion point
/// and call `emitBody` with the entity index, positioned inside the loop.
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
                function_ref<void(Value entity, Block *loopBody)> emitBody) {
  Value count = world.count(loc, archetype);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);

  if (archetype.capacity == 1) {
    Value present = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::sgt, count, zero);
    auto guard = scf::IfOp::create(rewriter, loc, present);
    rewriter.setInsertionPointToStart(guard.thenBlock());
    emitBody(zero, guard.thenBlock());
    return guard;
  }

  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  auto emitSequential = [&]() -> Operation * {
    auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVar(), loop.getBody());
    return loop;
  };
  auto emitParallel = [&]() -> Operation * {
    auto loop = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                        ValueRange{count}, ValueRange{one});
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVars().front(), loop.getBody());
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
    if (isa<GetOp, SetOp, ReadOp, AddOp, RemoveOp, EntityOp, LookupOp,
            YieldOp, scf::IfOp, scf::YieldOp>(op))
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

/// Replace the get/set/add/remove/despawn/entity ops nested in `roots` by
/// loads and stores at `entity` in the columns of `archetype`. With a
/// `mask`, every store keeps the old value where the mask is false: the
/// body ran for an entity it does not apply to.
static void lowerAccesses(IRRewriter &rewriter, ArrayRef<Operation *> roots,
                          const WorldArchetype &archetype, WorldAccess &world,
                          Value entity, Value mask) {
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

  SmallVector<Operation *> accesses;
  for (Operation *root : roots)
    root->walk([&](Operation *op) {
      if (isa<GetOp, SetOp, AddOp, RemoveOp, DespawnOp, EntityOp>(op))
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
        break;
      case ComponentChange::Overwrite:
        for (auto [value, field] :
             llvm::zip(values, componentOp.getFieldNames()))
          store(loc, value, column(component, cast<StringAttr>(field)));
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
    } else if (auto entityOp = dyn_cast<EntityOp>(op)) {
      Value id = memref::LoadOp::create(rewriter, loc, world.ids(archetype),
                                        ValueRange{entity});
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

/// Emit the body of `query` for `entity` of `archetype` at the insertion
/// point, with the query's parameters and outer values mapped by
/// `mapping`. If the query binds components that are optional in the
/// archetype, the body applies only where all of them are present: either
/// it runs for every entity and its stores are masked (branch-free, which
/// keeps the loop vectorisable), or, if the body cannot safely run for
/// absent entities, it is guarded by an `scf.if`.
static void emitQueryBody(IRRewriter &rewriter, QueryOp query,
                          IRMapping mapping, const WorldArchetype &archetype,
                          WorldAccess &world, Value entity) {
  Location loc = query.getLoc();
  ArchetypeOp archetypeOp = archetype.op;
  Value mask;
  for (Type type : query.getBody().getArgumentTypes()) {
    FlatSymbolRefAttr component = cast<RefType>(type).getComponent();
    if (!archetypeOp.isOptional(component))
      continue;
    Value presence =
        world.column(archetype, component.getAttr(), rewriter.getStringAttr(""));
    Value byte =
        memref::LoadOp::create(rewriter, loc, presence, ValueRange{entity});
    Value zero = arith::ConstantIntOp::create(rewriter, loc, 0, 8);
    Value present = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::ne, byte, zero);
    mask = mask ? arith::AndIOp::create(rewriter, loc, mask, present)
                : present;
  }

  OpBuilder::InsertionGuard guard(rewriter);
  if (mask && (!canRunForAbsentEntities(query) ||
               isStructuralFor(query, archetypeOp))) {
    auto branch = scf::IfOp::create(rewriter, loc, mask);
    rewriter.setInsertionPointToStart(branch.thenBlock());
    mask = Value();
  }
  SmallVector<Operation *> roots;
  for (Operation &op : query.getBody().front().without_terminator())
    roots.push_back(rewriter.clone(op, mapping));
  lowerAccesses(rewriter, roots, archetype, world, entity, mask);
}

/// Append the entity at `row` of `source` to `move.target`, as the move
/// says: shared components are copied, an added component takes the values
/// listed at pending slot `slot`, and optional components the entity did
/// not have in `source` start absent. Updates the entity's location.
static void applyMove(IRRewriter &rewriter, Location loc,
                      const WorldLayout &layout, const WorldArchetype &source,
                      const WorldMove &move, WorldAccess &world, Value row,
                      Value slot, Value id) {
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
    if (column.field.getValue().empty()) {
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
                         const WorldArchetype &archetype, WorldAccess &world) {
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
    if (!action) {
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
      applyMove(rewriter, loc, layout, archetype, move, world, row, slot, id);
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
        Value view = world.column(archetype, column.component, column.field);
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

/// Lower every ecs.spawn in `func`: check the capacity, write the values
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
    Value row = world.count(loc, *archetype);
    Value capacity =
        arith::ConstantIndexOp::create(rewriter, loc, archetype->capacity);
    Value fits = arith::CmpIOp::create(rewriter, loc, arith::CmpIPredicate::ult,
                                       row, capacity);
    cf::AssertOp::create(
        rewriter, loc, fits,
        rewriter.getStringAttr("ecs.spawn exceeds the capacity of @" +
                               spawn.getArchetypeAttr().getValue()));
    ArchetypeOp archetypeOp = archetype->op;
    auto value = spawn.getValues().begin();
    for (const WorldColumn &column : archetype->columns) {
      auto component = FlatSymbolRefAttr::get(column.component);
      Value stored;
      if (column.field.getValue().empty())
        stored = arith::ConstantIntOp::create(rewriter, loc, 0, 8);
      else if (!archetypeOp.isOptional(component))
        stored = world.toStorage(loc, *value++);
      else
        continue; // an absent optional component's fields stay as they are
      memref::StoreOp::create(
          rewriter, loc, stored,
          world.column(*archetype, column.component, column.field),
          ValueRange{row});
    }
    Value id = world.allocateEntity(loc, *archetype, row);
    memref::StoreOp::create(rewriter, loc, id, world.ids(*archetype),
                            ValueRange{row});
    Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
    world.setCount(loc, *archetype,
                   arith::AddIOp::create(rewriter, loc, row, one));
    rewriter.replaceOp(spawn, world.fromStorage(loc, id, spawn.getType()));
  }
}

/// Lower every ecs.lookup in `func` into guarded loads: is the id's slot
/// in use and its generation current; in which archetype and row does the
/// entity live; and, in each archetype holding the component, the field
/// (and, where the component is optional, its presence). Every load is
/// guarded, so a lookup is safe to run for any id.
static void lowerLookups(IRRewriter &rewriter, func::FuncOp func,
                         const WorldLayout &layout, WorldAccess &world) {
  SmallVector<LookupOp> lookups;
  func.walk([&](LookupOp lookup) { lookups.push_back(lookup); });
  for (LookupOp lookup : lookups) {
    Location loc = lookup.getLoc();
    rewriter.setInsertionPoint(lookup);
    Type type = world.storageType(lookup.getValue().getType());
    Type i1 = rewriter.getI1Type();
    SmallVector<Type, 2> resultTypes{type, i1};
    TypeRange results(resultTypes);
    auto missing = [&]() {
      Value zero = isa<FloatType>(type)
                       ? arith::ConstantOp::create(
                             rewriter, loc, rewriter.getFloatAttr(type, 0.0))
                             .getResult()
                       : arith::ConstantOp::create(
                             rewriter, loc, rewriter.getIntegerAttr(type, 0))
                             .getResult();
      Value no = arith::ConstantIntOp::create(rewriter, loc, 0, 1);
      scf::YieldOp::create(rewriter, loc, ValueRange{zero, no});
    };

    Value id = world.toStorage(loc, lookup.getEntity());
    Value slot = world.idSlot(loc, id);
    Value generation = world.idGeneration(loc, id);
    Value zeroIndex = arith::ConstantIndexOp::create(rewriter, loc, 0);
    Value used = world.toIndex(
        loc, memref::LoadOp::create(rewriter, loc,
                                    world.scalar(layout.nextSlotOffset),
                                    ValueRange{zeroIndex}));
    Value inRange = arith::CmpIOp::create(
        rewriter, loc, arith::CmpIPredicate::ult, slot, used);
    auto checkRange = scf::IfOp::create(rewriter, loc, results, inRange,
                                        /*withElseRegion=*/true);
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(checkRange.elseBlock());
      missing();
      rewriter.setInsertionPointToStart(checkRange.thenBlock());
      Value current = memref::LoadOp::create(rewriter, loc,
                                             world.generations(),
                                             ValueRange{slot});
      Value alive = arith::CmpIOp::create(
          rewriter, loc, arith::CmpIPredicate::eq, current, generation);
      auto checkAlive = scf::IfOp::create(rewriter, loc, results, alive,
                                          /*withElseRegion=*/true);
      scf::YieldOp::create(rewriter, loc, checkAlive.getResults());
      rewriter.setInsertionPointToStart(checkAlive.elseBlock());
      missing();
      rewriter.setInsertionPointToStart(checkAlive.thenBlock());
      auto [where, row] = world.getLocation(loc, slot);
      // One branch per archetype that holds the component.
      FlatSymbolRefAttr component = lookup.getComponentAttr();
      for (const WorldArchetype &archetype : layout.archetypes) {
        ArchetypeOp archetypeOp = archetype.op;
        if (!archetypeOp.contains(component))
          continue;
        Value here = arith::CmpIOp::create(
            rewriter, loc, arith::CmpIPredicate::eq, where,
            arith::ConstantIntOp::create(rewriter, loc, archetype.index,
                                         layout.entities.locationBits));
        auto branch = scf::IfOp::create(rewriter, loc, results, here,
                                        /*withElseRegion=*/true);
        scf::YieldOp::create(rewriter, loc, branch.getResults());
        rewriter.setInsertionPointToStart(branch.thenBlock());
        Value value = memref::LoadOp::create(
            rewriter, loc,
            world.column(archetype, component.getAttr(),
                         lookup.getFieldAttr()),
            ValueRange{row});
        Value found = arith::ConstantIntOp::create(rewriter, loc, 1, 1);
        if (archetypeOp.isOptional(component)) {
          Value byte = memref::LoadOp::create(
              rewriter, loc,
              world.column(archetype, component.getAttr(),
                           rewriter.getStringAttr("")),
              ValueRange{row});
          found = arith::CmpIOp::create(
              rewriter, loc, arith::CmpIPredicate::ne, byte,
              arith::ConstantIntOp::create(rewriter, loc, 0, 8));
        }
        scf::YieldOp::create(rewriter, loc, ValueRange{value, found});
        rewriter.setInsertionPointToStart(branch.elseBlock());
      }
      missing();
    }
    rewriter.setInsertionPointAfter(checkRange);
    rewriter.replaceOp(
        lookup, {world.fromStorage(loc, checkRange.getResult(0),
                                   lookup.getValue().getType()),
                 checkRange.getResult(1)});
  }
}

/// Replace a query by one loop per matching archetype. The body is cloned
/// into each loop, and every ref access becomes a load or store at the
/// loop's index in the column of the ref's component and field.
static void lowerQuery(IRRewriter &rewriter, QueryOp query,
                       const WorldLayout &layout, WorldAccess &world,
                       const LoopOptions &options) {
  Location loc = query.getLoc();
  bool entityLocal = isEntityLocal(query);
  bool matched = false;
  SmallVector<const WorldArchetype *> changed;
  for (const WorldArchetype &archetype : layout.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    rewriter.setInsertionPoint(query);
    Operation *loops = emitEntityLoops(
        rewriter, loc, archetype, world, options, entityLocal,
        [&](Value entity, Block *) {
          emitQueryBody(rewriter, query, IRMapping(), archetype, world,
                        entity);
        });
    hoistResourceReads(rewriter, loops, world);
    if (archetype.hasPending() && isStructuralFor(query, archetype.op))
      changed.push_back(&archetype);
  }

  // Despawns and moves take effect when the whole query has run, so an
  // entity moved into another archetype the query matches is not visited
  // twice.
  rewriter.setInsertionPoint(query);
  for (const WorldArchetype *archetype : changed)
    applyPending(rewriter, loc, layout, *archetype, world);

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
        [&](Value entity, Block *) {
          for (auto [query, index] : bodies)
            emitQueryBody(rewriter, query, mappings[index], archetype, world,
                          entity);
        });
    hoistResourceReads(rewriter, loops, world);
  }

  for (RunOp run : runs)
    rewriter.eraseOp(run);
}

/// Fuse every maximal sequence of runs of entity-local systems in a
/// schedule. Stages are dissolved first: fusion subsumes them. A run of an
/// opaque system, or an op with effects in the schedule body, ends a
/// sequence and stays where it is. So does a run of a system that writes a
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
      auto system = symbols.lookup<SystemOp>(run.getSystem());
      // Resource writes, structural changes and lookups end a sequence:
      // fusion interleaves systems per entity, and a lookup would then see
      // some entities' updates from other systems and not others'.
      bool writesResource =
          system
              .walk([](Operation *op) {
                return isa<WriteOp, SpawnOp, DespawnOp, LookupOp>(op)
                           ? WalkResult::interrupt()
                           : WalkResult::advance();
              })
              .wasInterrupted();
      for (QueryOp query : system.getBody().getOps<QueryOp>())
        for (ArchetypeOp archetype : getMatchedArchetypes(query))
          writesResource |= isStructuralFor(query, archetype);
      if (!writesResource && !computeAccess(system, archetypes).isOpaque()) {
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
  auto sections = omp::SectionsOp::create(rewriter, loc, omp::SectionsOperands{});
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

/// Replace every remaining !ecs.entity (function signatures, calls, scf
/// results, ops passing ids along) by the integer the layout stores ids as,
/// and fold away the casts the lowering placed at loads and stores.
static LogicalResult convertEntityTypes(ModuleOp module, unsigned idBits) {
  MLIRContext *context = module.getContext();
  auto isEntity = [](Type type) { return isa<EntityType>(type); };
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
struct EcsLowerToLoops
    : public mlir::ecs::impl::EcsLowerToLoopsBase<EcsLowerToLoops> {
  using EcsLowerToLoopsBase::EcsLowerToLoopsBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    IRRewriter rewriter(module.getContext());
    FailureOr<WorldLayout> layout = WorldLayout::compute(module);
    if (failed(layout))
      return signalPassFailure();
    MemRefType arenaType = getArenaType(module.getContext(), *layout);
    LoopOptions options{parallelEntities, parallelMinEntities};
    SymbolTable symbols(module);

    // Schedules first: fusion reads the systems' bodies before they are
    // lowered themselves.
    for (auto schedule :
         llvm::make_early_inc_range(module.getOps<ScheduleOp>())) {
      auto [func, arena] = convertToFunc(rewriter, schedule,
                                         schedule.getSymName(),
                                         schedule.getBody(), arenaType);
      func->setAttr("llvm.emit_c_interface", rewriter.getUnitAttr());
      WorldAccess world(rewriter, *layout, arena);
      if (fuseSystems)
        fuseSchedule(rewriter, func, symbols, *layout, world, options);
      SmallVector<RunOp> runs;
      func.walk([&](RunOp run) { runs.push_back(run); });
      for (RunOp run : runs) {
        SmallVector<Value> args(run.getArgs());
        args.push_back(arena);
        rewriter.setInsertionPoint(run);
        rewriter.replaceOpWithNewOp<func::CallOp>(run, run.getSystem(),
                                                  TypeRange{}, args);
      }
      SmallVector<StageOp> stages(func.getOps<StageOp>());
      for (StageOp stage : stages)
        lowerStage(rewriter, stage, parallelStages);
      lowerResourceAccesses(rewriter, func, world);
    }

    for (auto system : llvm::make_early_inc_range(module.getOps<SystemOp>())) {
      auto [func, arena] = convertToFunc(
          rewriter, system, system.getSymName(), system.getBody(), arenaType);
      func.setPrivate();
      WorldAccess world(rewriter, *layout, arena);
      SmallVector<QueryOp> queries;
      func.walk([&](QueryOp query) { queries.push_back(query); });
      for (QueryOp query : queries)
        lowerQuery(rewriter, query, *layout, world, options);
      lowerSpawns(rewriter, func, *layout, world);
      lowerLookups(rewriter, func, *layout, world);
      lowerResourceAccesses(rewriter, func, world);
    }

    for (Operation &op : llvm::make_early_inc_range(module.getOps()))
      if (isa<ComponentOp, ResourceOp, ArchetypeOp>(op))
        rewriter.eraseOp(&op);

    if (failed(convertEntityTypes(module, layout->entities.idBits)))
      return signalPassFailure();
  }
};
} // namespace
