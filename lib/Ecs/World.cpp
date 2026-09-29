#include "Ecs/World.h"
#include "Ecs/Structure.h"

#include "llvm/ADT/SmallPtrSet.h"
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

const WorldResourceField *WorldResource::find(StringAttr field) const {
  for (const WorldResourceField &entry : fields)
    if (entry.field == field)
      return &entry;
  return nullptr;
}

const WorldMove *WorldArchetype::findMove(StringAttr component,
                                          bool add) const {
  for (const WorldMove &move : moves)
    if (move.component == component && move.add == add)
      return &move;
  return nullptr;
}

const WorldResource &WorldLayout::getResource(StringAttr resource) const {
  for (const WorldResource &entry : resources)
    if (ResourceOp(entry.op).getSymNameAttr() == resource)
      return entry;
  llvm_unreachable("resource is not part of the layout");
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
  for (ResourceOp resource : module.getOps<ResourceOp>()) {
    WorldResource entry;
    entry.op = resource;
    end = llvm::alignTo(end, kColumnAlignment);
    for (auto [name, typeAttr] :
         llvm::zip(resource.getFieldNames(), resource.getFieldTypes())) {
      Type type = cast<TypeAttr>(typeAttr).getValue();
      uint64_t bytes = getStorageBytes(type);
      if (bytes == 0)
        return resource.emitOpError("field ")
               << name << " has type " << type
               << ", which world storage does not support";
      uint64_t offset = llvm::alignTo(end, bytes);
      entry.fields.push_back({cast<StringAttr>(name), type, offset});
      end = offset + bytes;
    }
    layout.resources.push_back(std::move(entry));
  }
  // Archetypes that some query despawns from or moves entities out of need
  // pending lists and a counter for the rows to remove when that query
  // ends; moves also need to know their target and, for an add, the values.
  llvm::SmallPtrSet<Operation *, 4> despawned;
  module.walk([&](DespawnOp despawn) {
    for (ArchetypeOp archetype :
         getMatchedArchetypes(despawn->getParentOfType<QueryOp>()))
      despawned.insert(archetype);
  });
  auto recordMoves = [&](Operation *op, FlatSymbolRefAttr component,
                         bool add) {
    for (ArchetypeOp source :
         getMatchedArchetypes(op->getParentOfType<QueryOp>())) {
      ComponentChange change = classifyChange(source, component, add);
      if (change.kind != ComponentChange::Move)
        continue;
      for (WorldArchetype &archetype : layout.archetypes)
        if (archetype.op == source &&
            !archetype.findMove(component.getAttr(), add))
          archetype.moves.push_back({change.target, component.getAttr(), add,
                                     unsigned(archetype.moves.size() + 1),
                                     {}});
    }
  };
  module.walk([&](AddOp add) {
    recordMoves(add, add.getComponentAttr(), /*add=*/true);
  });
  module.walk([&](RemoveOp remove) {
    recordMoves(remove, remove.getComponentAttr(), /*add=*/false);
  });
  auto needsPending = [&](const WorldArchetype &archetype) {
    return despawned.contains(archetype.op) || !archetype.moves.empty();
  };
  for (WorldArchetype &archetype : layout.archetypes)
    if (needsPending(archetype)) {
      end = llvm::alignTo(end, 8);
      archetype.pendingCountOffset = end;
      end += 8;
    }
  end = llvm::alignTo(end, 8);
  layout.nextSlotOffset = end;
  layout.freeCountOffset = end + 8;
  end += 16;
  layout.headerBytes = end;

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
      // An optional component also gets a presence byte per entity, as a
      // column with an empty field name.
      if (archetype.op.isOptional(cast<FlatSymbolRefAttr>(attr))) {
        uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
        archetype.columns.push_back(
            {componentName, StringAttr::get(module.getContext(), ""),
             IntegerType::get(module.getContext(), 8), offset});
        end = offset + archetype.capacity;
      }
    }
    auto place = [&](uint64_t bytes) {
      uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
      end = offset + bytes * archetype.capacity;
      return offset;
    };
    archetype.idOffset = place(8);
    if (needsPending(archetype))
      archetype.pendingOffset = place(4);
    if (!archetype.moves.empty()) {
      archetype.pendingActionOffset = place(4);
      for (WorldMove &move : archetype.moves) {
        if (!move.add)
          continue;
        auto component = SymbolTable::lookupNearestSymbolFrom<ComponentOp>(
            module, FlatSymbolRefAttr::get(move.component));
        for (auto [name, typeAttr] : llvm::zip(component.getFieldNames(),
                                               component.getFieldTypes())) {
          Type type = cast<TypeAttr>(typeAttr).getValue();
          move.values.push_back({move.component, cast<StringAttr>(name), type,
                                 place(getStorageBytes(type))});
        }
      }
    }
    layout.entityCapacity += archetype.capacity;
  }

  // The entity table.
  auto placeTable = [&](uint64_t bytes) {
    uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
    end = offset + bytes * layout.entityCapacity;
    return offset;
  };
  layout.generationOffset = placeTable(4);
  layout.locationArchetypeOffset = placeTable(4);
  layout.locationRowOffset = placeTable(4);
  layout.freeListOffset = placeTable(4);
  layout.totalBytes = llvm::alignTo(end, kArenaAlignment);
  return layout;
}
