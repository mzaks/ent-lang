#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
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

/// Whether `system`, but for `query`, may change the field `query`'s
/// order is by, or its key equals (`component`, `field`), or which
/// entities there are before it runs: then the index, made when the
/// system starts, is not what the `for` would sort. (Writes to the key in
/// the query itself are fine: a `for ... by` goes by the values the key
/// had when it started.)
bool changesOrderOutside(SystemOp system, QueryOp query,
                         FlatSymbolRefAttr component, StringAttr field) {
  // (What comes after the query does not change what it sees, where no
  // loop of the system's runs the two again.)
  bool once = true;
  for (Operation *around = query->getParentOp(); around != system;
       around = around->getParentOp())
    once &= !isa<scf::ForOp, scf::WhileOp>(around);
  WalkResult result = system.walk<WalkOrder::PreOrder>([&](Operation *op) {
    if (op == query.getOperation())
      return WalkResult::skip();
    if (Operation *top = query->getBlock()->findAncestorOpInBlock(*op);
        once && top && query->isBeforeInBlock(top))
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

/// Whether `system` writes the key of the index `kept` or a field it
/// copies (of entities that are there: not in spawning them), or hands
/// a proc one of their components. (Not a unique it is made of, such as
/// a grid's cell: the GPU is given those as it runs.)
bool writesValues(SystemOp system, const Kept &kept) {
  auto held = [&](Attribute component, StringAttr field) {
    return llvm::any_of(kept.values, [&](const Column &column) {
      return std::get<0>(column) && std::get<1>(column) == component &&
             (!field || std::get<2>(column) == field);
    });
  };
  WalkResult result = system.walk([&](Operation *op) {
    if (auto set = dyn_cast<SetOp>(op))
      if (held(cast<RefType>(set.getRef().getType()).getComponent().getAttr(),
               set.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto apply = dyn_cast<ApplyOp>(op))
      if (held(apply.getComponentAttr().getAttr(), apply.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto combine = dyn_cast<CombineOp>(op))
      if (held(cast<RefType>(combine.getRef().getType())
                   .getComponent()
                   .getAttr(),
               combine.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto invoke = dyn_cast<InvokeOp>(op); invoke && invoke.getProc())
      for (Value operand : invoke->getOperands())
        if (auto ref = dyn_cast<RefType>(operand.getType());
            ref && held(ref.getComponent().getAttr(), StringAttr()))
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
    // (Clustered ones first: the runs that move their rows then change
    // what the others hold, and those see them.)
    llvm::stable_sort(indexes, [](auto &a, auto &b) {
      return a.second.get("clustered") && !b.second.get("clustered");
    });
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

    // An index with a form made on the GPU (a Morton index's) is made so
    // where the program is built for one, all the runs that read it are of
    // gpu systems, and no system on the CPU writes its key or what it
    // copies (spawns do not: those are new entities, which the GPU's form
    // makes anew anyway): its rows are then never wanted on the CPU, nor
    // copied there and back. The other form is left out. (Clustered, its
    // rows moved on the GPU, where the program has no relation, whose
    // trees may keep where their entities are: those are sorted again on
    // the CPU.)
    bool relations = !module.getOps<RelationOp>().empty();
    for (auto &[buffer, info] : indexes) {
      auto onGpu = info.getAs<StringAttr>("gpu_update");
      if (!onGpu)
        continue;
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      bool chosen = gpu && (!info.get("clustered") || !relations);
      bool read = false;
      if (chosen)
        module.walk([&](RunOp run) {
          StringAttr name = run.getSystemAttr().getAttr();
          if (name == kept.update || name == kept.stale || name == kept.moved ||
              name == onGpu)
            return;
          Operation *declared = symbols.lookup(run.getSystem());
          const SystemAccess &access = accessOf(run);
          if (isa<ExternOp>(declared) && access.isOpaque()) {
            chosen = false;
            return;
          }
          auto system = dyn_cast<SystemOp>(declared);
          if (touches(access.reads, kept.readBy)) {
            read = true;
            chosen &= system && system.getGpu();
          }
          if (system && !system.getGpu() && writesValues(system, kept))
            chosen = false;
        });
      Operation *unused = symbols.lookup(chosen && read ? kept.update : onGpu);
      if (chosen && read) {
        NamedAttrList changed(info);
        changed.set("update", onGpu);
        info = changed.getDictionary(module.getContext());
        buffer->setAttr("ent.index", info);
      }
      symbols.erase(unused);
    }

    // A `for` that goes by the field an index is by, over the entities it
    // holds, takes its order from it (QueryOp::kByIndexAttr): the same
    // order, without the sort; and so reads it. Only where the program
    // reads the index itself, which keeps it up to date anyway: made only
    // for such a `for`, the index costs more than the sort it saves where
    // keys change much (made anew, and its rows written and told), and
    // where few change it is made again on one thread where the sort runs
    // on many (bench/RESULTS.md, 2026-10-23).
    // (Not in what makes an index: one is made of the entities as they
    // are, not of another index, which may be out of date then.)
    llvm::DenseSet<Attribute> making;
    for (auto [buffer, info] : indexes)
      making.insert(info.get("update"));
    for (auto [buffer, info] : indexes) {
      // (A hash index has no order.)
      if (info.get("hash"))
        continue;
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
      module.walk([&](QueryOp query) {
        auto system = query->getParentOfType<SystemOp>();
        if (!system || making.contains(system.getSymNameAttr()) ||
            !query.hasByField() || query.getByIndex() ||
            query.getByComponent().getAttr() != key[0] ||
            query.getByField() != key[1])
          return;
        SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
        if (matched.size() != held.size() ||
            !llvm::all_of(matched, [&](ArchetypeOp archetype) {
              return held.contains(archetype.getSymNameAttr());
            }))
          return;
        if (changesOrderOutside(system, query, query.getByComponent(),
                                query.getByField()))
          return;
        // (Nor where its body spawns: the index does not have those.)
        if (query.getBody()
                .walk([](SpawnOp) { return WalkResult::interrupt(); })
                .wasInterrupted())
          return;
        query->setAttr(QueryOp::kByIndexAttr,
                       FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
      });
    }

    // A `for` whose `where` compares a field with a value the same for
    // all its entities (QueryOp::kKeyEqualsAttr), over the entities a
    // hash index by that field holds, visits only those it says have the
    // value (QueryOp::kViaHashAttr): in their order, as kept in the index
    // (its place), the `where` still asked. Not where the system changes
    // the key or which entities there are, but in the `for` itself and
    // there only of the entity visited (written or sent); nor where the
    // body spawns, adds or removes.
    for (auto [buffer, info] : indexes) {
      if (!info.get("hash"))
        continue;
      SmallVector<StringAttr> key = strings(info, "key");
      SmallVector<StringAttr> of = strings(info, "of");
      StringAttr place;
      for (StringAttr hidden : strings(info, "hidden"))
        if (hidden.getValue().starts_with("place_in_"))
          place = hidden;
      if (!place)
        continue;
      llvm::SmallDenseSet<Attribute> held;
      for (ArchetypeOp archetype : archetypes)
        if (llvm::all_of(of, [&](StringAttr component) {
              return archetype.contains(FlatSymbolRefAttr::get(component)) &&
                     !archetype.isOptional(FlatSymbolRefAttr::get(component));
            }))
          held.insert(archetype.getSymNameAttr());
      module.walk([&](QueryOp query) {
        auto equals = query->getAttrOfType<ArrayAttr>(QueryOp::kKeyEqualsAttr);
        auto system = query->getParentOfType<SystemOp>();
        // (Nor one over a set's entities, which it finds by the set.)
        if (!equals || query.getViaHash() || query.getInSet() || !system ||
            system.getGpu() || making.contains(system.getSymNameAttr()))
          return;
        auto component = cast<FlatSymbolRefAttr>(equals[0]);
        auto field = cast<StringAttr>(equals[1]);
        if (component.getAttr() != key[0] || field != key[1])
          return;
        SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
        if (matched.size() != held.size() ||
            !llvm::all_of(matched, [&](ArchetypeOp archetype) {
              return held.contains(archetype.getSymNameAttr());
            }))
          return;
        if (changesOrderOutside(system, query, component, field))
          return;
        Block &body = query.getBody().front();
        WalkResult inside = query.getBody().walk([&](Operation *op) {
          if (isa<SpawnOp, AddOp, RemoveOp>(op))
            return WalkResult::interrupt();
          // (The key written, or sent: of the entity visited only.)
          auto other = [&](Value ref, StringAttr written) {
            return cast<RefType>(ref.getType()).getComponent() == component &&
                   written == field && ref.getParentBlock() != &body;
          };
          if (auto set = dyn_cast<SetOp>(op);
              set && other(set.getRef(), set.getFieldAttr()))
            return WalkResult::interrupt();
          if (auto combine = dyn_cast<CombineOp>(op);
              combine && other(combine.getRef(), combine.getFieldAttr()))
            return WalkResult::interrupt();
          if (auto apply = dyn_cast<ApplyOp>(op);
              apply && apply.getComponentAttr() == component &&
              apply.getFieldAttr() == field)
            return WalkResult::interrupt();
          return WalkResult::advance();
        });
        if (inside.wasInterrupted())
          return;
        query->setAttr(QueryOp::kViaHashAttr,
                       FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
        query->setAttr(QueryOp::kByAttr,
                       ArrayAttr::get(module.getContext(), {component, place}));
      });
    }
    module.walk([&](QueryOp query) {
      if (!query.getViaHash())
        query.dropEquals();
    });

    // The systems that keep the indexes that are not clustered: each
    // writes only fields of its own (where an entity's row is, its
    // place) and its flags, which no other index holds; but it hands a
    // proc the first component to change, which the analysis takes for
    // writing all of it (and so for moving its entities). A clustered
    // one's update moves rows: that one changes the others.
    llvm::DenseSet<Attribute> keepers;
    for (auto [buffer, info] : indexes)
      if (!info.get("clustered"))
        for (StringRef which : {"update", "stale", "moved"})
          keepers.insert(info.get(which));

    OpBuilder builder(module.getContext());
    for (auto [buffer, info] : indexes) {
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      SmallVector<RunOp> runs;
      module.walk([&](RunOp run) { runs.push_back(run); });
      for (RunOp run : runs) {
        StringAttr system = run.getSystemAttr().getAttr();
        if (system == kept.update || system == kept.stale ||
            system == kept.moved || keepers.contains(system))
          continue;
        const SystemAccess &access = accessOf(run);
        // (A system with a body that calls out is opaque to the schedule,
        // but its world is what its body does; only an extern system
        // that declares nothing may do anything to it.)
        bool unknown = access.isOpaque() &&
                       isa<ExternOp>(symbols.lookup(run.getSystem()));
        // (A clustered index is made before every run where it is out of
        // date: its archetype's rows are in its order when any system
        // starts.)
        bool reads = unknown || touches(access.reads, kept.readBy) ||
                     info.get("clustered");
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
          auto marked = RunOp::create(
              builder, loc,
              FlatSymbolRefAttr::get(moves ? kept.moved : kept.stale),
              ValueRange{});
          // Eager: made again right after what changed it, where it is
          // out of date (not where that run did not run).
          if (info.get("eager")) {
            builder.setInsertionPointAfter(marked);
            auto update =
                RunOp::create(builder, loc, FlatSymbolRefAttr::get(kept.update),
                              ValueRange{});
            Block *condition = builder.createBlock(&update.getCondition());
            builder.setInsertionPointToStart(condition);
            Value fresh = ReadOp::create(builder, loc, builder.getI1Type(),
                                         FlatSymbolRefAttr::get(kept.fresh),
                                         builder.getStringAttr("value"));
            Value yes = arith::ConstantIntOp::create(builder, loc, 1, 1);
            YieldOp::create(
                builder, loc,
                ValueRange{arith::XOrIOp::create(builder, loc, fresh, yes)});
          }
        }
      }
    }
  }
};

} // namespace
