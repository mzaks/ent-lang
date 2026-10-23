#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTINDEXES
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// What one index is made of and what reads it, as columns, and the
/// systems that keep it.
struct Kept {
  StringAttr update, stale, moved, fresh;
  /// Which entities it holds, and the values it holds of them.
  llvm::SetVector<Column> holds;
  llvm::SetVector<Column> values;
  llvm::SetVector<Column> readBy;
};

/// The strings of an array attribute.
SmallVector<StringAttr> strings(DictionaryAttr info, StringRef name) {
  SmallVector<StringAttr> list;
  for (Attribute item : cast<ArrayAttr>(info.get(name)))
    list.push_back(cast<StringAttr>(item));
  return list;
}

Kept keptOf(BufferOp buffer, DictionaryAttr info,
            ArrayRef<ArchetypeOp> archetypes, SymbolTable &symbols) {
  MLIRContext *context = buffer.getContext();
  StringAttr empty = StringAttr::get(context, "");
  Kept kept;
  kept.update = cast<StringAttr>(info.get("update"));
  kept.stale = cast<StringAttr>(info.get("stale"));
  kept.moved = cast<StringAttr>(info.get("moved"));
  kept.fresh = cast<StringAttr>(info.get("fresh"));
  SmallVector<StringAttr> of = strings(info, "of");
  SmallVector<StringAttr> row = strings(info, "row");
  // What reads it: its rows (and a grid's cells and sizes), and the
  // fields where the entities keep their place in it.
  for (StringAttr own : strings(info, "own")) {
    Operation *declared = symbols.lookup(own);
    if (isa_and_nonnull<BufferOp>(declared))
      kept.readBy.insert(
          {StringAttr(), own, StringAttr::get(context, "rows")});
    if (auto resource = dyn_cast_or_null<ResourceOp>(declared))
      for (Attribute field : resource.getFieldNames())
        kept.readBy.insert({StringAttr(), own, cast<StringAttr>(field)});
  }
  for (ArchetypeOp archetype : archetypes) {
    StringAttr name = archetype.getSymNameAttr();
    if (archetype.contains(FlatSymbolRefAttr::get(row[0])))
      for (StringAttr hidden : strings(info, "hidden"))
        kept.readBy.insert({name, row[0], hidden});
    if (!llvm::all_of(of, [&](StringAttr component) {
          return archetype.contains(FlatSymbolRefAttr::get(component));
        }))
      continue;
    // Which entities it holds.
    kept.holds.insert({name, empty, empty});
    for (StringAttr component : of)
      if (archetype.isOptional(FlatSymbolRefAttr::get(component)))
        kept.holds.insert({name, component, empty});
  }
  // The values it holds of them: the fields its update reads, of the
  // components (but those it keeps its own in) and of the uniques it
  // does not write itself (but the flags that say it is up to date): the
  // key, what it copies, a grid's places and what its sizes are worked
  // out of. (Its update gives a proc the first component to change, which
  // the analysis takes for writing all of it.)
  SystemAccess made = computeAccess(symbols.lookup(kept.update), archetypes);
  for (const Column &column : made.reads) {
    auto [archetype, component, field] = column;
    if (kept.readBy.contains(column) || kept.holds.contains(column) ||
        !component || component.empty() || !field || field.empty() ||
        (!archetype && made.writes.contains(column)))
      continue;
    kept.values.insert(column);
  }
  return kept;
}

/// Whether `system`, but for `query`, may change what `query`'s order is
/// by or which entities there are before it runs: then the index, made
/// when the system starts, is not what the `for` would sort. (Writes to
/// the key in the query itself are fine: a `for ... by` goes by the values
/// the key had when it started.)
bool changesOrderOutside(SystemOp system, QueryOp query) {
  FlatSymbolRefAttr component = query.getByComponent();
  StringAttr field = query.getByField();
  WalkResult result = system.walk([&](Operation *op) {
    if (op == query.getOperation())
      return WalkResult::skip();
    if (isa<SpawnOp, DespawnOp, AddOp, RemoveOp>(op))
      return WalkResult::interrupt();
    if (auto invoke = dyn_cast<InvokeOp>(op); invoke && invoke.getProc())
      return WalkResult::interrupt();
    if (auto set = dyn_cast<SetOp>(op))
      if (cast<RefType>(set.getRef().getType()).getComponent() == component &&
          set.getFieldAttr() == field)
        return WalkResult::interrupt();
    if (auto apply = dyn_cast<ApplyOp>(op))
      if (apply.getComponentAttr() == component &&
          apply.getFieldAttr() == field)
        return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return result.wasInterrupted();
}

bool touches(const llvm::SetVector<Column> &accessed,
             const llvm::SetVector<Column> &columns) {
  return llvm::any_of(columns,
                      [&](const Column &c) { return accessed.contains(c); });
}

struct EntIndexes : public mlir::ent::impl::EntIndexesBase<EntIndexes> {
  using EntIndexesBase::EntIndexesBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    SmallVector<std::pair<BufferOp, DictionaryAttr>> indexes;
    for (BufferOp buffer : module.getOps<BufferOp>())
      if (auto info = buffer->getAttrOfType<DictionaryAttr>("ent.index"))
        indexes.push_back({buffer, info});
    if (indexes.empty())
      return;
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    SmallVector<ArchetypeOp> archetypes(module.getOps<ArchetypeOp>());
    SymbolTable symbols(module);
    llvm::DenseMap<Operation *, SystemAccess> accesses;
    auto accessOf = [&](RunOp run) -> const SystemAccess & {
      Operation *system = symbols.lookup(run.getSystem());
      auto [it, inserted] = accesses.try_emplace(system);
      if (inserted)
        it->second = computeAccess(system, archetypes);
      return it->second;
    };

    // A `for` that goes by the field an index is by, over the entities it
    // holds, takes its order from it (QueryOp::kByIndexAttr): the same
    // order, without the sort; and so reads it. Only where the program
    // reads the index itself, which keeps it up to date anyway: made only
    // for such a `for`, the index costs more than the sort it saves where
    // keys change much (made anew, and its rows written and told), and
    // where few change it is made again on one thread where the sort runs
    // on many (bench/RESULTS.md, 2026-10-23).
    for (auto [buffer, info] : indexes) {
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      bool read = false;
      for (SystemOp system : module.getOps<SystemOp>()) {
        StringAttr name = system.getSymNameAttr();
        if (name == kept.update || name == kept.stale || name == kept.moved)
          continue;
        read |= touches(computeAccess(system, archetypes).reads, kept.readBy);
      }
      if (!read)
        continue;
      SmallVector<StringAttr> key = strings(info, "key");
      SmallVector<StringAttr> of = strings(info, "of");
      llvm::SmallDenseSet<Attribute> held;
      for (ArchetypeOp archetype : archetypes)
        if (llvm::all_of(of, [&](StringAttr component) {
              return archetype.contains(FlatSymbolRefAttr::get(component)) &&
                     !archetype.isOptional(FlatSymbolRefAttr::get(component));
            }))
          held.insert(archetype.getSymNameAttr());
      StringAttr update = cast<StringAttr>(info.get("update"));
      module.walk([&](QueryOp query) {
        auto system = query->getParentOfType<SystemOp>();
        // (Not the index's own making, which sorts the entities into it.)
        if (!system || system.getSymNameAttr() == update || !query.hasBy() ||
            query.getByIndex() || query.getByComponent().getAttr() != key[0] ||
            query.getByField() != key[1])
          return;
        SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
        if (matched.size() != held.size() ||
            !llvm::all_of(matched, [&](ArchetypeOp archetype) {
              return held.contains(archetype.getSymNameAttr());
            }))
          return;
        if (changesOrderOutside(system, query))
          return;
        query->setAttr(QueryOp::kByIndexAttr,
                       FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
      });
    }

    OpBuilder builder(module.getContext());
    for (auto [buffer, info] : indexes) {
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      SmallVector<RunOp> runs;
      module.walk([&](RunOp run) { runs.push_back(run); });
      for (RunOp run : runs) {
        StringAttr system = run.getSystemAttr().getAttr();
        if (system == kept.update || system == kept.stale ||
            system == kept.moved)
          continue;
        const SystemAccess &access = accessOf(run);
        // (A system with a body that calls out is opaque to the schedule,
        // but its world is what its body does; only an extern system
        // that declares nothing may do anything to it.)
        bool unknown = access.isOpaque() &&
                       isa<ExternOp>(symbols.lookup(run.getSystem()));
        bool reads = unknown || touches(access.reads, kept.readBy);
        bool moves = unknown || touches(access.writes, kept.holds);
        bool writes = moves || touches(access.writes, kept.values);
        Location loc = run.getLoc();
        if (reads) {
          builder.setInsertionPoint(run);
          auto update = RunOp::create(builder, loc,
                                      FlatSymbolRefAttr::get(kept.update),
                                      ValueRange{});
          Block *condition = builder.createBlock(&update.getCondition());
          builder.setInsertionPointToStart(condition);
          Value fresh = ReadOp::create(builder, loc, builder.getI1Type(),
                                       FlatSymbolRefAttr::get(kept.fresh),
                                       builder.getStringAttr("value"));
          Value yes = arith::ConstantIntOp::create(builder, loc, 1, 1);
          Value stale = arith::XOrIOp::create(builder, loc, fresh, yes);
          // And only where what reads it runs: on its condition too.
          if (!run.getCondition().empty()) {
            IRMapping mapping;
            Block &asked = run.getCondition().front();
            for (Operation &op : asked.without_terminator())
              builder.clone(op, mapping);
            Value runs = mapping.lookupOrDefault(
                asked.getTerminator()->getOperand(0));
            stale = arith::AndIOp::create(builder, loc, stale, runs);
          }
          YieldOp::create(builder, loc, ValueRange{stale});
        }
        if (writes) {
          builder.setInsertionPointAfter(run);
          RunOp::create(builder, loc,
                        FlatSymbolRefAttr::get(moves ? kept.moved
                                                     : kept.stale),
                        ValueRange{});
        }
      }
    }
  }
};

} // namespace
