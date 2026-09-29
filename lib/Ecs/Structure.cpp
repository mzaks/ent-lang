#include "Ecs/Structure.h"

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
