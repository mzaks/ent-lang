#include "Ecs/Structure.h"

#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;
using namespace mlir::ecs;

SmallVector<ArchetypeOp> mlir::ecs::getMatchedArchetypes(QueryOp query) {
  SmallVector<ArchetypeOp> matched;
  auto module = query->getParentOfType<ModuleOp>();
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>())
    if (llvm::all_of(query.getBody().getArgumentTypes(), [&](Type type) {
          return archetype.contains(cast<RefType>(type).getComponent());
        }))
      matched.push_back(archetype);
  return matched;
}

/// The archetype whose components are exactly `components`, or null.
static ArchetypeOp findArchetype(ModuleOp module,
                                 const llvm::SmallPtrSet<Attribute, 8> &wanted) {
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>()) {
    ArrayAttr components = archetype.getComponents();
    if (components.size() == wanted.size() &&
        llvm::all_of(components,
                     [&](Attribute attr) { return wanted.contains(attr); }))
      return archetype;
  }
  return {};
}

ComponentChange mlir::ecs::classifyChange(ArchetypeOp archetype,
                                          FlatSymbolRefAttr component,
                                          bool add) {
  if (archetype.isOptional(component))
    return {ComponentChange::Presence, {}};
  bool holds = archetype.contains(component);
  if (add && holds)
    return {ComponentChange::Overwrite, {}};
  if (!add && !holds)
    return {ComponentChange::Nothing, {}};

  llvm::SmallPtrSet<Attribute, 8> wanted;
  for (Attribute attr : archetype.getComponents())
    wanted.insert(attr);
  if (add)
    wanted.insert(component);
  else
    wanted.erase(component);
  ArchetypeOp target =
      findArchetype(archetype->getParentOfType<ModuleOp>(), wanted);
  if (!target)
    return {ComponentChange::NoTarget, {}};
  return {ComponentChange::Move, target};
}

SmallVector<Trigger> mlir::ecs::getTriggers(QueryOp query) {
  SmallVector<Trigger> triggers;
  auto list = query->getAttrOfType<ArrayAttr>(QueryOp::kTriggersAttr);
  if (!list)
    return triggers;
  for (Attribute attr : list) {
    auto entry = cast<ArrayAttr>(attr);
    StringRef kind = cast<StringAttr>(entry[0]).getValue();
    std::optional<int64_t> logCapacity;
    if (entry.size() > 3)
      logCapacity = cast<IntegerAttr>(entry[3]).getInt();
    triggers.push_back({kind == "added"     ? Trigger::Added
                        : kind == "removed" ? Trigger::Removed
                                            : Trigger::Changed,
                        cast<FlatSymbolRefAttr>(entry[1]),
                        cast<StringAttr>(entry[2]), logCapacity});
  }
  return triggers;
}

Stamp mlir::ecs::getStamp(const Trigger &trigger) {
  return {trigger.kind, trigger.component.getAttr(), trigger.field};
}

/// Whether `stamp` means something in `archetype`.
static bool isMeaningful(ArchetypeOp archetype, const Stamp &stamp) {
  auto component = FlatSymbolRefAttr::get(stamp.component);
  if (stamp.kind == Trigger::Removed)
    return !archetype.contains(component) || archetype.isOptional(component);
  return archetype.contains(component);
}

StampPlan StampPlan::compute(ModuleOp module) {
  StampPlan plan;
  // Where a reactive query observes each stamp.
  SmallVector<SmallVector<ArchetypeOp>> observers;
  module.walk([&](QueryOp query) {
    for (const Trigger &trigger : getTriggers(query)) {
      Stamp stamp = getStamp(trigger);
      auto *it = llvm::find(plan.stamps, stamp);
      size_t index = it - plan.stamps.begin();
      if (it == plan.stamps.end()) {
        plan.stamps.push_back(stamp);
        observers.emplace_back();
      }
      for (ArchetypeOp archetype : getMatchedArchetypes(query))
        observers[index].push_back(archetype);
    }
  });
  if (plan.stamps.empty())
    return plan;

  // Moves the program can make: source -> targets.
  llvm::DenseMap<Operation *, SmallVector<ArchetypeOp>> moves;
  auto recordMoves = [&](Operation *op, FlatSymbolRefAttr component,
                         bool add) {
    for (ArchetypeOp source :
         getMatchedArchetypes(op->getParentOfType<QueryOp>())) {
      ComponentChange change = classifyChange(source, component, add);
      if (change.kind == ComponentChange::Move)
        moves[source].push_back(change.target);
    }
  };
  module.walk([&](AddOp add) {
    recordMoves(add, add.getComponentAttr(), /*add=*/true);
  });
  module.walk([&](RemoveOp remove) {
    recordMoves(remove, remove.getComponentAttr(), /*add=*/false);
  });

  // Per stamp: the observed archetypes where it means something, then
  // backwards along moves between archetypes where it means something.
  for (auto [stamp, observed] : llvm::zip(plan.stamps, observers)) {
    llvm::SmallPtrSet<Operation *, 4> storing;
    for (ArchetypeOp archetype : observed)
      if (isMeaningful(archetype, stamp))
        storing.insert(archetype);
    bool grew = true;
    while (grew) {
      grew = false;
      for (auto &[source, targets] : moves) {
        auto sourceOp = cast<ArchetypeOp>(source);
        if (storing.contains(source) || !isMeaningful(sourceOp, stamp))
          continue;
        if (llvm::any_of(targets, [&](ArchetypeOp target) {
              return storing.contains(target);
            })) {
          storing.insert(source);
          grew = true;
        }
      }
    }
    plan.storing.push_back(std::move(storing));
  }
  return plan;
}

bool StampPlan::stores(ArchetypeOp archetype, const Stamp &stamp) const {
  auto *it = llvm::find(stamps, stamp);
  if (it == stamps.end())
    return false;
  return storing[it - stamps.begin()].contains(archetype);
}

LogicalResult mlir::ecs::inferArchetypes(ModuleOp module) {
  // Spawn shapes still without an archetype, as component sets in
  // declaration order.
  SmallVector<ComponentOp> declared(module.getOps<ComponentOp>());
  auto inOrder = [&](const llvm::SmallPtrSetImpl<Attribute> &set) {
    SmallVector<Attribute> ordered;
    for (ComponentOp component : declared) {
      auto ref = FlatSymbolRefAttr::get(component.getSymNameAttr());
      if (set.contains(ref))
        ordered.push_back(ref);
    }
    return ordered;
  };
  llvm::MapVector<ArrayAttr, SmallVector<SpawnOp>> shapes;
  Builder builder(module.getContext());
  module.walk([&](SpawnOp spawn) {
    if (spawn.getArchetypeAttr() || !spawn.getComponentsAttr())
      return;
    llvm::SmallPtrSet<Attribute, 8> set(spawn.getComponentsAttr().begin(),
                                        spawn.getComponentsAttr().end());
    shapes[builder.getArrayAttr(inOrder(set))].push_back(spawn);
  });
  if (shapes.empty())
    return success();

  // What queries add and remove, and the components they bind.
  struct Change {
    llvm::SmallPtrSet<Attribute, 4> bound;
    Attribute component;
    bool add;
  };
  SmallVector<Change> changes;
  module.walk([&](Operation *op) {
    if (!isa<AddOp, RemoveOp>(op))
      return;
    Change change;
    for (Type type :
         op->getParentOfType<QueryOp>().getBody().getArgumentTypes())
      change.bound.insert(cast<RefType>(type).getComponent());
    change.component = op->getAttr("component");
    change.add = isa<AddOp>(op);
    changes.push_back(std::move(change));
  });

  SymbolTable symbols(module);
  auto defaultCapacity =
      module->getAttrOfType<IntegerAttr>("ecs.default_capacity");
  // New archetypes go after the last declaration they may refer to.
  Operation *after = nullptr;
  for (Operation &op : module.getOps())
    if (isa<ComponentOp, ArchetypeOp>(op))
      after = &op;
  OpBuilder insert(module.getContext());
  insert.setInsertionPointAfter(after);

  for (auto &[listed, spawns] : shapes) {
    llvm::SmallPtrSet<Attribute, 8> base(listed.begin(), listed.end());
    // A declared archetype with exactly these required components.
    ArchetypeOp chosen;
    for (ArchetypeOp archetype : module.getOps<ArchetypeOp>()) {
      llvm::SmallPtrSet<Attribute, 8> required;
      for (Attribute attr : archetype.getComponents())
        if (!archetype.isOptional(cast<FlatSymbolRefAttr>(attr)))
          required.insert(attr);
      if (required.size() == base.size() &&
          llvm::all_of(required, [&](Attribute component) {
            return base.contains(component);
          })) {
        chosen = archetype;
        break;
      }
    }

    if (!chosen) {
      // Optional members: what adds and removes reach, until nothing
      // changes.
      llvm::SmallPtrSet<Attribute, 8> optional;
      for (bool grew = true; grew;) {
        grew = false;
        for (const Change &change : changes) {
          bool matches = llvm::all_of(change.bound, [&](Attribute component) {
            return base.contains(component) || optional.contains(component);
          });
          if (!matches || optional.contains(change.component))
            continue;
          if (change.add ? !base.contains(change.component)
                         : base.contains(change.component)) {
            optional.insert(change.component);
            grew = true;
          }
        }
      }
      llvm::SmallPtrSet<Attribute, 8> all(base.begin(), base.end());
      all.insert(optional.begin(), optional.end());

      // Capacity: the smallest among the required components.
      std::optional<int64_t> capacity;
      for (Attribute component : listed) {
        if (optional.contains(component))
          continue;
        auto componentOp = symbols.lookup<ComponentOp>(
            cast<FlatSymbolRefAttr>(component).getAttr());
        if (std::optional<int64_t> limit = componentOp.getCapacity())
          capacity = std::min(capacity.value_or(*limit), *limit);
      }
      if (!capacity && defaultCapacity)
        capacity = defaultCapacity.getInt();

      std::string name;
      for (Attribute component : listed) {
        if (!name.empty())
          name += "_";
        name += cast<FlatSymbolRefAttr>(component).getValue();
      }
      if (symbols.lookup(name))
        name += "_archetype";
      if (symbols.lookup(name))
        return spawns.front().emitOpError("needs an archetype named @")
               << name << ", but that name is taken";
      if (!capacity) {
        InFlightDiagnostic diag =
            spawns.front().emitOpError("spawns into an archetype (@")
            << name << ") with no capacity: give one of its required "
            << "components a capacity, or set ecs.default_capacity on the "
               "module";
        return diag;
      }

      SmallVector<Attribute> optionalList = inOrder(optional);
      chosen = ArchetypeOp::create(
          insert, spawns.front().getLoc(), insert.getStringAttr(name),
          insert.getArrayAttr(inOrder(all)),
          optionalList.empty() ? ArrayAttr()
                               : insert.getArrayAttr(optionalList),
          insert.getI64IntegerAttr(*capacity), insert.getUnitAttr());
      symbols.insert(chosen);
      insert.setInsertionPointAfter(chosen);
    }
    for (SpawnOp spawn : spawns)
      spawn.setArchetypeAttr(
          FlatSymbolRefAttr::get(chosen.getSymNameAttr()));
  }
  return success();
}
