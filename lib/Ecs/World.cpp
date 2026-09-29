#include "Ecs/World.h"

#include "llvm/Support/MathExtras.h"

using namespace mlir;
using namespace mlir::ecs;

uint64_t mlir::ecs::getStorageBytes(Type type) {
  if (isa<IndexType>(type))
    return 8;
  if (auto integer = dyn_cast<IntegerType>(type)) {
    switch (integer.getWidth()) {
    case 1:
    case 8:
      return 1;
    case 16:
      return 2;
    case 32:
      return 4;
    case 64:
      return 8;
    default:
      return 0;
    }
  }
  if (type.isF32())
    return 4;
  if (type.isF64())
    return 8;
  return 0;
}

const WorldColumn *WorldArchetype::find(StringAttr component,
                                        StringAttr field) const {
  for (const WorldColumn &column : columns)
    if (column.component == component && column.field == field)
      return &column;
  return nullptr;
}

FailureOr<WorldLayout> WorldLayout::compute(ModuleOp module) {
  SymbolTable symbols(module);
  WorldLayout layout;
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>()) {
    WorldArchetype entry;
    entry.op = archetype;
    entry.capacity = archetype.getCapacity();
    entry.index = layout.archetypes.size();
    layout.archetypes.push_back(std::move(entry));
  }
  layout.countsBytes = 8 * layout.archetypes.size();

  uint64_t end = layout.countsBytes;
  for (WorldArchetype &archetype : layout.archetypes) {
    for (Attribute attr : archetype.op.getComponents()) {
      auto componentName = cast<FlatSymbolRefAttr>(attr).getAttr();
      auto component = symbols.lookup<ComponentOp>(componentName);
      for (auto [name, typeAttr] :
           llvm::zip(component.getFieldNames(), component.getFieldTypes())) {
        Type type = cast<TypeAttr>(typeAttr).getValue();
        uint64_t bytes = getStorageBytes(type);
        if (bytes == 0)
          return component.emitOpError("field ")
                 << name << " has type " << type
                 << ", which world storage does not support";
        uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
        archetype.columns.push_back(
            {componentName, cast<StringAttr>(name), type, offset});
        end = offset + bytes * archetype.capacity;
      }
    }
  }
  layout.totalBytes = llvm::alignTo(end, kArenaAlignment);
  return layout;
}
