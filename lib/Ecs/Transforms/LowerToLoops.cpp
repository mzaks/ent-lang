#include "Ecs/Access.h"
#include "Ecs/EcsOps.h"
#include "Ecs/Passes.h"
#include "Ecs/World.h"

#include "mlir/IR/IRMapping.h"
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

  /// The number of entities in `archetype`, as an index.
  Value count(const WorldArchetype &archetype) {
    Value &value = counts[archetype.index];
    if (!value)
      value = atEntry([&](Location loc) {
        if (!countsView) {
          auto countsType = MemRefType::get(
              {static_cast<int64_t>(layout.archetypes.size())},
              IntegerType::get(rewriter.getContext(), 64));
          Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
          countsView = memref::ViewOp::create(rewriter, loc, countsType, arena,
                                              zero, ValueRange{});
        }
        Value position =
            arith::ConstantIndexOp::create(rewriter, loc, archetype.index);
        Value count = memref::LoadOp::create(rewriter, loc, countsView,
                                             ValueRange{position});
        return arith::IndexCastOp::create(rewriter, loc,
                                          rewriter.getIndexType(), count);
      });
    return value;
  }

  /// A statically shaped view of one column of `archetype`.
  Value column(const WorldArchetype &archetype, StringAttr component,
               StringAttr field) {
    const WorldColumn *column = archetype.find(component, field);
    Value &value = columns[{archetype.index, column->offset}];
    if (!value)
      value = atEntry([&](Location loc) {
        auto type = MemRefType::get({archetype.capacity}, column->type);
        Value offset =
            arith::ConstantIndexOp::create(rewriter, loc, column->offset);
        return memref::ViewOp::create(rewriter, loc, type, arena, offset,
                                      ValueRange{});
      });
    return value;
  }

private:
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
  llvm::DenseMap<unsigned, Value> counts;
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

/// True if the query body only computes and accesses its own entity's
/// components, so its iterations are independent of each other.
static bool isEntityLocal(QueryOp query) {
  WalkResult result = query.getBody().walk([](Operation *op) {
    if (isa<GetOp, SetOp, YieldOp>(op) || !hasOwnEffects(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !result.wasInterrupted();
}

/// Emit the loop over the entities of `archetype` at the insertion point
/// and call `emitBody` with the entity index, positioned inside the loop.
///
/// Iterations of entity-local bodies are independent, so the loop may be an
/// `scf.parallel`, but a parallel loop only pays for its fork beyond some
/// number of entities. An archetype whose capacity is below that threshold
/// never gets a parallel loop; otherwise the count is checked at run time
/// and the body is emitted twice, once per loop kind.
static void
emitEntityLoops(IRRewriter &rewriter, Location loc,
                const WorldArchetype &archetype, WorldAccess &world,
                const LoopOptions &options, bool entityLocal,
                function_ref<void(Value entity, Block *loopBody)> emitBody) {
  Value count = world.count(archetype);
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);

  auto emitSequential = [&] {
    auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVar(), loop.getBody());
  };
  auto emitParallel = [&] {
    auto loop = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                        ValueRange{count}, ValueRange{one});
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    emitBody(loop.getInductionVars().front(), loop.getBody());
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
}

/// Clone a query's body at the insertion point. Its get/set ops still name
/// the query's refs; `lowerAccesses` resolves them afterwards.
static void cloneQueryBody(IRRewriter &rewriter, QueryOp query,
                           IRMapping mapping) {
  for (Operation &op : query.getBody().front().without_terminator())
    rewriter.clone(op, mapping);
}

/// Replace every get/set nested in `loopBody` by a load or store at
/// `entity` in the column of the ref's component and field in `archetype`.
static void lowerAccesses(IRRewriter &rewriter, Block *loopBody,
                          const WorldArchetype &archetype, WorldAccess &world,
                          Value entity) {
  auto column = [&](Value ref, StringAttr field) {
    FlatSymbolRefAttr component = cast<RefType>(ref.getType()).getComponent();
    return world.column(archetype, component.getAttr(), field);
  };
  SmallVector<Operation *> accesses;
  loopBody->walk([&](Operation *op) {
    if (isa<GetOp, SetOp>(op))
      accesses.push_back(op);
  });
  for (Operation *op : accesses) {
    rewriter.setInsertionPoint(op);
    if (auto get = dyn_cast<GetOp>(op)) {
      rewriter.replaceOpWithNewOp<memref::LoadOp>(
          get, column(get.getRef(), get.getFieldAttr()), ValueRange{entity});
    } else {
      auto set = cast<SetOp>(op);
      rewriter.replaceOpWithNewOp<memref::StoreOp>(
          set, set.getValue(), column(set.getRef(), set.getFieldAttr()),
          ValueRange{entity});
    }
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
  for (const WorldArchetype &archetype : layout.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    rewriter.setInsertionPoint(query);
    emitEntityLoops(rewriter, loc, archetype, world, options, entityLocal,
                    [&](Value entity, Block *loopBody) {
                      cloneQueryBody(rewriter, query, IRMapping());
                      lowerAccesses(rewriter, loopBody, archetype, world,
                                    entity);
                    });
  }

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
    emitEntityLoops(rewriter, loc, archetype, world, options,
                    /*entityLocal=*/true, [&](Value entity, Block *loopBody) {
                      for (auto [query, index] : bodies)
                        cloneQueryBody(rewriter, query, mappings[index]);
                      lowerAccesses(rewriter, loopBody, archetype, world,
                                    entity);
                    });
  }

  for (RunOp run : runs)
    rewriter.eraseOp(run);
}

/// Fuse every maximal sequence of runs of entity-local systems in a
/// schedule. Stages are dissolved first: fusion subsumes them. A run of an
/// opaque system, or an op with effects in the schedule body, ends a
/// sequence and stays where it is.
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
      if (!computeAccess(system, archetypes).isOpaque()) {
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
    }

    for (Operation &op : llvm::make_early_inc_range(module.getOps()))
      if (isa<ComponentOp, ArchetypeOp>(op))
        rewriter.eraseOp(&op);
  }
};
} // namespace
