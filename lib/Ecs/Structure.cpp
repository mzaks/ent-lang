#include "Ecs/Structure.h"

#include "llvm/ADT/DenseMap.h"
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
    triggers.push_back({kind == "added"     ? Trigger::Added
                        : kind == "removed" ? Trigger::Removed
                                            : Trigger::Changed,
                        cast<FlatSymbolRefAttr>(entry[1]),
                        cast<StringAttr>(entry[2])});
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
