#include "Ecs/Access.h"

#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;
using namespace mlir::ecs;

std::string mlir::ecs::formatColumn(const Column &column) {
  auto [archetype, component, field] = column;
  return (archetype.getValue() + "." + component.getValue() + "." +
          field.getValue())
      .str();
}

bool mlir::ecs::hasOwnEffects(Operation *op) {
  if (op->hasTrait<OpTrait::HasRecursiveMemoryEffects>())
    return false;
  if (auto effects = dyn_cast<MemoryEffectOpInterface>(op))
    return !effects.hasNoEffect();
  return true;
}

std::optional<std::string>
SystemAccess::conflictWith(const SystemAccess &other,
                           StringRef otherName) const {
  auto effects = [](Operation *op) {
    return "has effects outside component access ('" +
           op->getName().getStringRef().str() + "')";
  };
  if (isOpaque())
    return "it " + effects(opaqueOp);
  if (other.isOpaque())
    return "@" + otherName.str() + " " + effects(other.opaqueOp);
  for (const Column &column : writes) {
    if (other.writes.contains(column))
      return "it writes " + formatColumn(column) + ", which @" +
             otherName.str() + " also writes";
    if (other.reads.contains(column))
      return "it writes " + formatColumn(column) + ", which @" +
             otherName.str() + " reads";
  }
  for (const Column &column : reads)
    if (other.writes.contains(column))
      return "it reads " + formatColumn(column) + ", which @" +
             otherName.str() + " writes";
  return std::nullopt;
}

SystemAccess mlir::ecs::computeAccess(SystemOp system,
                                      ArrayRef<ArchetypeOp> archetypes) {
  SystemAccess access;
  llvm::DenseMap<Operation *, SmallVector<StringAttr>> matched;

  auto matchedArchetypes = [&](QueryOp query) -> ArrayRef<StringAttr> {
    auto [it, inserted] = matched.try_emplace(query);
    if (inserted) {
      for (ArchetypeOp archetype : archetypes) {
        bool all = llvm::all_of(
            query.getBody().getArgumentTypes(), [&](Type type) {
              return archetype.contains(cast<RefType>(type).getComponent());
            });
        if (all)
          it->second.push_back(archetype.getSymNameAttr());
      }
    }
    return it->second;
  };

  auto record = [&](Operation *op, Value ref, StringAttr field,
                    llvm::SetVector<Column> &into) {
    auto query = op->getParentOfType<QueryOp>();
    StringAttr component = cast<RefType>(ref.getType()).getComponent().getAttr();
    for (StringAttr archetype : matchedArchetypes(query))
      into.insert({archetype, component, field});
  };

  system.getBody().walk([&](Operation *op) {
    if (auto get = dyn_cast<GetOp>(op))
      return record(op, get.getRef(), get.getFieldAttr(), access.reads);
    if (auto set = dyn_cast<SetOp>(op))
      return record(op, set.getRef(), set.getFieldAttr(), access.writes);
    if (isa<QueryOp, YieldOp>(op))
      return;
    if (!access.opaqueOp && hasOwnEffects(op))
      access.opaqueOp = op;
  });
  return access;
}
