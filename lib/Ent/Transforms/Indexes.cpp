#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Builders.h"
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
            ArrayRef<ArchetypeOp> archetypes) {
  MLIRContext *context = buffer.getContext();
  StringAttr empty = StringAttr::get(context, "");
  Kept kept;
  kept.update = cast<StringAttr>(info.get("update"));
  kept.stale = cast<StringAttr>(info.get("stale"));
  kept.moved = cast<StringAttr>(info.get("moved"));
  kept.fresh = cast<StringAttr>(info.get("fresh"));
  SmallVector<StringAttr> of = strings(info, "of");
  SmallVector<StringAttr> key = strings(info, "key");
  SmallVector<StringAttr> copies = strings(info, "copies");
  SmallVector<StringAttr> row = strings(info, "row");
  kept.readBy.insert({StringAttr(), buffer.getSymNameAttr(),
                      StringAttr::get(context, "rows")});
  for (ArchetypeOp archetype : archetypes) {
    StringAttr name = archetype.getSymNameAttr();
    if (archetype.contains(FlatSymbolRefAttr::get(row[0])))
      kept.readBy.insert({name, row[0], row[1]});
    if (!llvm::all_of(of, [&](StringAttr component) {
          return archetype.contains(FlatSymbolRefAttr::get(component));
        }))
      continue;
    // Which entities it holds, their keys and what it copies.
    kept.holds.insert({name, empty, empty});
    for (StringAttr component : of)
      if (archetype.isOptional(FlatSymbolRefAttr::get(component)))
        kept.holds.insert({name, component, empty});
    kept.values.insert({name, key[0], key[1]});
    for (unsigned i = 0; i + 1 < copies.size(); i += 2)
      kept.values.insert({name, copies[i], copies[i + 1]});
  }
  return kept;
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

    OpBuilder builder(module.getContext());
    for (auto [buffer, info] : indexes) {
      Kept kept = keptOf(buffer, info, archetypes);
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
