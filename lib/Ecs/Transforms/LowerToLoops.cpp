#include "Ecs/Access.h"
#include "Ecs/EcsOps.h"
#include "Ecs/Passes.h"

#include "mlir/IR/IRMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ecs {
#define GEN_PASS_DEF_ECSLOWERTOLOOPS
#include "Ecs/Passes.h.inc"
} // namespace mlir::ecs

using namespace mlir;
using namespace mlir::ecs;

namespace {

/// Where one archetype's storage sits among the world arguments.
struct ArchetypeLayout {
  ArchetypeOp op;
  /// Position of the entity count.
  unsigned count = 0;
  /// (component symbol, field name) -> position of the field's column.
  llvm::DenseMap<std::pair<Attribute, Attribute>, unsigned> columns;
};

/// The world ABI: the argument list every lowered function takes after its
/// own parameters. See the pass description for the order.
struct WorldLayout {
  SmallVector<Type> types;
  SmallVector<ArchetypeLayout> archetypes;
};

} // namespace

static WorldLayout buildWorldLayout(ModuleOp module) {
  SymbolTable symbols(module);
  WorldLayout world;
  Type index = IndexType::get(module.getContext());
  for (auto archetype : module.getOps<ArchetypeOp>()) {
    ArchetypeLayout layout;
    layout.op = archetype;
    layout.count = world.types.size();
    world.types.push_back(index);
    for (Attribute attr : archetype.getComponents()) {
      auto ref = cast<FlatSymbolRefAttr>(attr);
      auto component = symbols.lookup<ComponentOp>(ref.getValue());
      for (auto [name, type] :
           llvm::zip(component.getFieldNames(), component.getFieldTypes())) {
        layout.columns[{ref, name}] = world.types.size();
        world.types.push_back(MemRefType::get(
            {ShapedType::kDynamic}, cast<TypeAttr>(type).getValue()));
      }
    }
    world.archetypes.push_back(std::move(layout));
  }
  return world;
}

/// Replace `op` (a system or schedule) by a function whose entry block is
/// `op`'s body, extended by the world arguments. Returns the function and,
/// through `worldArgs`, the values that hold the world inside it.
static func::FuncOp convertToFunc(IRRewriter &rewriter, Operation *op,
                                  StringRef name, Region &body,
                                  const WorldLayout &world,
                                  SmallVectorImpl<Value> &worldArgs) {
  Block &entry = body.front();
  SmallVector<Type> inputs(entry.getArgumentTypes());
  llvm::append_range(inputs, world.types);

  rewriter.setInsertionPoint(op);
  auto func = func::FuncOp::create(rewriter, op->getLoc(), name,
                                   rewriter.getFunctionType(inputs, {}));
  rewriter.inlineRegionBefore(body, func.getBody(), func.getBody().end());
  for (Type type : world.types)
    worldArgs.push_back(entry.addArgument(type, op->getLoc()));

  Operation *terminator = entry.getTerminator();
  rewriter.setInsertionPoint(terminator);
  rewriter.replaceOpWithNewOp<func::ReturnOp>(terminator);
  rewriter.eraseOp(op);
  return func;
}

static bool matches(const ArchetypeLayout &archetype, QueryOp query) {
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

/// Create a loop over the entities of `archetype` and set the insertion
/// point into its body. Iterations of a query only touch their own entity,
/// so the loop may be an `scf.parallel`.
static Value createEntityLoop(IRRewriter &rewriter, Location loc,
                              const ArchetypeLayout &archetype,
                              ValueRange worldArgs, bool parallel) {
  Value zero = arith::ConstantIndexOp::create(rewriter, loc, 0);
  Value one = arith::ConstantIndexOp::create(rewriter, loc, 1);
  Value count = worldArgs[archetype.count];
  if (parallel) {
    auto loop = scf::ParallelOp::create(rewriter, loc, ValueRange{zero},
                                        ValueRange{count}, ValueRange{one});
    rewriter.setInsertionPoint(loop.getBody()->getTerminator());
    return loop.getInductionVars().front();
  }
  auto loop = scf::ForOp::create(rewriter, loc, zero, count, one);
  rewriter.setInsertionPoint(loop.getBody()->getTerminator());
  return loop.getInductionVar();
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
                          const ArchetypeLayout &archetype,
                          ValueRange worldArgs, Value entity) {
  auto column = [&](Value ref, StringAttr field) {
    FlatSymbolRefAttr component = cast<RefType>(ref.getType()).getComponent();
    return worldArgs[archetype.columns.lookup({component, field})];
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
                       const WorldLayout &world, ValueRange worldArgs,
                       bool parallelEntities) {
  Location loc = query.getLoc();
  bool parallel = parallelEntities && isEntityLocal(query);
  bool matched = false;
  for (const ArchetypeLayout &archetype : world.archetypes) {
    if (!matches(archetype, query))
      continue;
    matched = true;
    rewriter.setInsertionPoint(query);
    Value entity =
        createEntityLoop(rewriter, loc, archetype, worldArgs, parallel);
    Block *loopBody = rewriter.getInsertionBlock();
    cloneQueryBody(rewriter, query, IRMapping());
    lowerAccesses(rewriter, loopBody, archetype, worldArgs, entity);
  }

  // The set of archetypes is closed, so a query that matches none of them
  // can never run; that is almost certainly a mistake in the program.
  if (!matched)
    query.emitWarning("matches no archetype; the query is removed");
  rewriter.eraseOp(query);
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
    WorldLayout world = buildWorldLayout(module);

    for (auto system : llvm::make_early_inc_range(module.getOps<SystemOp>())) {
      SmallVector<Value> worldArgs;
      auto func = convertToFunc(rewriter, system, system.getSymName(),
                                system.getBody(), world, worldArgs);
      func.setPrivate();
      SmallVector<QueryOp> queries;
      func.walk([&](QueryOp query) { queries.push_back(query); });
      for (QueryOp query : queries)
        lowerQuery(rewriter, query, world, worldArgs, parallelEntities);
    }

    for (auto schedule :
         llvm::make_early_inc_range(module.getOps<ScheduleOp>())) {
      SmallVector<Value> worldArgs;
      auto func = convertToFunc(rewriter, schedule, schedule.getSymName(),
                                schedule.getBody(), world, worldArgs);
      func->setAttr("llvm.emit_c_interface", rewriter.getUnitAttr());
      SmallVector<RunOp> runs;
      func.walk([&](RunOp run) { runs.push_back(run); });
      for (RunOp run : runs) {
        SmallVector<Value> args(run.getArgs());
        llvm::append_range(args, worldArgs);
        rewriter.setInsertionPoint(run);
        rewriter.replaceOpWithNewOp<func::CallOp>(run, run.getSystem(),
                                                  TypeRange{}, args);
      }
      SmallVector<StageOp> stages(func.getOps<StageOp>());
      for (StageOp stage : stages)
        lowerStage(rewriter, stage, parallelStages);
    }

    for (Operation &op : llvm::make_early_inc_range(module.getOps()))
      if (isa<ComponentOp, ArchetypeOp>(op))
        rewriter.eraseOp(&op);
  }
};
} // namespace
