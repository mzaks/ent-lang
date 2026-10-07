#include "Ent/World.h"
#include "Ent/Structure.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MathExtras.h"

using namespace mlir;
using namespace mlir::ent;

uint64_t mlir::ent::getStorageBytes(Type type) {
  if (isa<IndexType, EntityType>(type))
    return 8;
  if (auto text = dyn_cast<TextType>(type))
    return text.getStorageBytes();
  if (isa<EnumType>(type))
    return 1;
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
    if (!column.isStamp() && column.component == component &&
        column.field == field)
      return &column;
  return nullptr;
}

const WorldColumn *WorldArchetype::findStamp(const Stamp &stamp) const {
  for (const WorldColumn &column : columns)
    if (column.stamp && *column.stamp == stamp)
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

const WorldApplyBuffer &WorldApply::find(unsigned archetype) const {
  for (const WorldApplyBuffer &buffer : buffers)
    if (buffer.archetype == archetype)
      return buffer;
  llvm_unreachable("the apply's query does not match the archetype");
}

const WorldColumn *WorldRelation::find(StringAttr field) const {
  for (const WorldColumn &column : fields)
    if (column.field == field)
      return &column;
  return nullptr;
}

const WorldConnect::Buffer &WorldConnect::find(unsigned archetype) const {
  for (const Buffer &buffer : buffers)
    if (buffer.archetype == archetype)
      return buffer;
  llvm_unreachable("the connect's query does not match the archetype");
}

bool WorldLayout::cascadeFollowsEvents(QueryOp query) const {
  SmallVector<Trigger> triggers = getTriggers(query);
  if (!query.getCascade() || query.isLeavesFirst() || triggers.empty())
    return false;
  FlatSymbolRefAttr cascade = query.getCascade();
  const WorldRelation &tree = getRelation(cascade.getAttr());
  if (!(tree.linked || !tree.sortedArchetypes.empty()) ||
      llvm::none_of(query.getBody().getArgumentTypes(), [&](Type type) {
        auto ref = dyn_cast<RefType>(type);
        return ref && ref.getVia() == cascade;
      }))
    return false;
  for (const Trigger &trigger : triggers) {
    if (!findLog(getStamp(trigger)))
      return false;
    if (trigger.via && tree.getTrusted(/*target=*/true) != trigger.component)
      return false;
  }
  return true;
}

const WorldRelation &WorldLayout::getRelation(StringAttr relation) const {
  for (const WorldRelation &entry : relations)
    if (RelationOp(entry.op).getSymNameAttr() == relation)
      return entry;
  llvm_unreachable("relation is not part of the layout");
}

const WorldLog *WorldLayout::findLog(const Stamp &stamp) const {
  ArrayRef<Stamp> all = stamps.getStamps();
  auto *it = llvm::find(all, stamp);
  if (it == all.end() || !logs[it - all.begin()].exists())
    return nullptr;
  return &logs[it - all.begin()];
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
  // Choose how ids are represented, before the columns: relation fields
  // are stored at the id's width.
  EntityScheme &scheme = layout.entities;
  int64_t maxCapacity = 1;
  for (const WorldArchetype &archetype : layout.archetypes) {
    maxCapacity = std::max(maxCapacity, archetype.capacity);
    layout.entityCapacity += archetype.capacity;
  }
  scheme.rowBits = std::max(1u, llvm::Log2_64_Ceil(maxCapacity));
  unsigned archetypeBits =
      std::max(1u, llvm::Log2_64_Ceil(layout.archetypes.size()));
  scheme.locationBits = scheme.rowBits + archetypeBits <= 32 ? 32 : 64;
  // An archetype sorted by a tree moves its rows too.
  for (RelationOp relation : module.getOps<RelationOp>()) {
    if (!relation.getSorted())
      continue;
    FailureOr<SmallVector<ArchetypeOp>> sorted =
        getSortedArchetypes(relation);
    if (failed(sorted))
      return failure();
    for (WorldArchetype &archetype : layout.archetypes)
      if (llvm::is_contained(*sorted, ArchetypeOp(archetype.op)))
        archetype.sortedBy = relation.getSymNameAttr();
  }
  bool moves = llvm::any_of(layout.archetypes, [](const WorldArchetype &a) {
    return !a.moves.empty() || a.isSorted();
  });
  auto attr = [&](StringRef name, int64_t otherwise) {
    if (auto value = module->getAttrOfType<IntegerAttr>(name))
      return value.getInt();
    return otherwise;
  };
  bool wide = attr("ent.entity_id_bits", 0) == 64;
  unsigned minGenerationBits = attr("ent.min_generation_bits", 8);
  scheme.slotBits = std::max(1u, llvm::Log2_64_Ceil(layout.entityCapacity));
  if (despawned.empty() && !moves) {
    scheme.kind = EntityScheme::Rows;
    scheme.idBits = !wide && scheme.locationBits == 32 ? 32 : 64;
    scheme.generationBits = scheme.generationStorageBits = 0;
  } else if (despawned.empty()) {
    scheme.kind = EntityScheme::Slots;
    scheme.idBits = !wide && scheme.slotBits <= 32 ? 32 : 64;
    scheme.generationBits = scheme.generationStorageBits = 0;
  } else {
    scheme.kind = EntityScheme::Generational;
    if (!wide && scheme.slotBits + minGenerationBits <= 32) {
      scheme.idBits = 32;
      scheme.generationBits = 32 - scheme.slotBits;
    } else {
      scheme.idBits = 64;
      scheme.generationBits = std::min(32u, 64 - scheme.slotBits);
    }
    scheme.generationStorageBits = scheme.generationBits <= 8    ? 8
                                   : scheme.generationBits <= 16 ? 16
                                                                 : 32;
  }

  layout.entityKeys =
      scheme.kind == EntityScheme::Rows
          ? int64_t(layout.archetypes.size()) << scheme.rowBits
          : layout.entityCapacity;

  // Bytes a field of `type` takes; ids take the scheme's width.
  auto storageBytes = [&](Type type) -> uint64_t {
    if (isa<EntityType>(type))
      return scheme.idBits / 8;
    return getStorageBytes(type);
  };

  for (ResourceOp resource : module.getOps<ResourceOp>()) {
    WorldResource entry;
    entry.op = resource;
    end = llvm::alignTo(end, kColumnAlignment);
    for (auto [name, typeAttr] :
         llvm::zip(resource.getFieldNames(), resource.getFieldTypes())) {
      Type type = cast<TypeAttr>(typeAttr).getValue();
      uint64_t bytes = storageBytes(type);
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
  auto needsPending = [&](const WorldArchetype &archetype) {
    return despawned.contains(archetype.op) || !archetype.moves.empty();
  };
  for (WorldArchetype &archetype : layout.archetypes)
    if (needsPending(archetype)) {
      end = llvm::alignTo(end, 8);
      archetype.pendingCountOffset = end;
      end += 8;
    }
  for (WorldArchetype &archetype : layout.archetypes)
    if (archetype.isSorted()) {
      end = llvm::alignTo(end, 8);
      archetype.rootCountOffset = end;
      end += 8;
    }
  // Relations: their edge counts and clean flags.
  for (RelationOp relation : module.getOps<RelationOp>()) {
    WorldRelation entry;
    entry.op = relation;
    entry.capacity = relation.getCapacity();
    end = llvm::alignTo(end, 8);
    entry.countOffset = end;
    entry.cleanOffset = end + 8;
    end += 16;
    entry.tree = relation.getTree();
    entry.linked = relation.getTree() && !relation.getSorted();
    for (bool target : {false, true})
      entry.trusted[target] = getTrustedEndpoint(relation, target);
    for (const WorldArchetype &archetype : layout.archetypes)
      if (archetype.sortedBy == relation.getSymNameAttr())
        entry.sortedArchetypes.push_back(archetype.index);
    if (!entry.sortedArchetypes.empty()) {
      entry.depthOffset = end;
      end += 8;
    }
    if (entry.tree) {
      entry.orderCountOffset = end;
      end += 8;
    }
    if (entry.tree && !entry.linked) {
      entry.sortedCountOffset = end;
      end += 8;
    }
    if (entry.linked && scheme.hasIds()) {
      entry.staleOffset = end;
      end += 8;
    }
    layout.relations.push_back(std::move(entry));
  }
  end = llvm::alignTo(end, 8);
  layout.nextSlotOffset = end;
  layout.freeHeadOffset = end + 8;
  end += 16;
  // Reactive queries: the tick counter and each query's last tick.
  layout.stamps = StampPlan::compute(module);
  module.walk([&](QueryOp query) {
    if (getTriggers(query).empty())
      return;
    if (!layout.tickOffset) {
      layout.tickOffset = end;
      end += 8;
    }
    layout.reactiveOffsets.push_back(end);
    end += 8;
  });
  // Event logs: a capacity per stamp, the largest any trigger asks for
  // (`log N`), or by default an eighth of the entities that can carry the
  // stamp; rounded up to a power of two so positions wrap with a mask.
  ArrayRef<Stamp> stamps = layout.stamps.getStamps();
  SmallVector<std::optional<int64_t>> asked(stamps.size());
  module.walk([&](QueryOp query) {
    for (const Trigger &trigger : getTriggers(query)) {
      if (!trigger.logCapacity)
        continue;
      auto &entry = asked[llvm::find(stamps, getStamp(trigger)) -
                          stamps.begin()];
      entry = std::max(entry.value_or(0), *trigger.logCapacity);
    }
  });
  for (auto [stamp, request] : llvm::zip(stamps, asked)) {
    WorldLog log;
    int64_t entities = 0;
    for (const WorldArchetype &archetype : layout.archetypes)
      if (layout.stamps.stores(archetype.op, stamp))
        entities += archetype.capacity;
    // (Connects: of as many entities as the relation has edges.)
    if (stamp.kind == Trigger::Connected)
      entities = layout.getRelation(stamp.component).capacity;
    int64_t capacity = request ? *request : std::max<int64_t>(64, entities / 8);
    if (capacity > 0) {
      log.capacity = int64_t(llvm::PowerOf2Ceil(uint64_t(capacity)));
      log.segments = std::clamp<int64_t>(
          log.capacity / WorldLog::kMinSegmentCapacity, 1,
          WorldLog::kMaxSegments);
      log.segmentCapacity = log.capacity / log.segments;
      end = llvm::alignTo(end, WorldLog::kSegmentStride);
      log.countsOffset = end;
      end += log.segments * WorldLog::kSegmentStride;
      log.endsOffset = end;
      end += 8 * log.segments;
    }
    layout.logs.push_back(std::move(log));
  }
  module.walk([&](QueryOp query) {
    SmallVector<Trigger> triggers = getTriggers(query);
    if (triggers.empty())
      return;
    SmallVector<uint64_t> positions;
    for (const Trigger &trigger : triggers) {
      WorldLog &log =
          layout.logs[llvm::find(stamps, getStamp(trigger)) - stamps.begin()];
      // (A cascading query that goes through its whole tree reads no
      // log.)
      bool scans = query.getCascade() && !layout.cascadeFollowsEvents(query);
      if (!log.exists() || scans) {
        positions.push_back(0);
        continue;
      }
      log.readerOffsets.push_back(end);
      positions.push_back(end);
      end += 8 * log.segments;
    }
    layout.readPositions.push_back(std::move(positions));
  });
  layout.headerBytes = end;

  for (WorldArchetype &archetype : layout.archetypes) {
    for (Attribute attr : archetype.op.getComponents()) {
      auto componentName = cast<FlatSymbolRefAttr>(attr).getAttr();
      auto component = symbols.lookup<ComponentOp>(componentName);
      for (auto [name, typeAttr] :
           llvm::zip(component.getFieldNames(), component.getFieldTypes())) {
        Type type = cast<TypeAttr>(typeAttr).getValue();
        uint64_t bytes = storageBytes(type);
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
    for (const Stamp &stamp : layout.stamps.getStamps())
      if (layout.stamps.stores(archetype.op, stamp))
        archetype.columns.push_back(
            {stamp.component, StringAttr::get(module.getContext(), ""),
             IntegerType::get(module.getContext(), 64), place(8), stamp});
    if (scheme.hasIds())
      archetype.idOffset = place(scheme.idBits / 8);
    if (needsPending(archetype))
      archetype.pendingOffset = place(4);
    if (archetype.isSorted()) {
      // Alone in its tree, or one of several archetypes.
      int64_t others = 0, sources = 0;
      for (const WorldArchetype &other : layout.archetypes)
        if (other.sortedBy == archetype.sortedBy) {
          ++others;
          sources += other.capacity;
        }
      if (llvm::is_contained(
              layout.stamps.getStamps(),
              Stamp{Trigger::Connected, archetype.sortedBy,
                    StringAttr::get(module.getContext(), "")}))
        archetype.connectedRowOffset = place(8);
      if (others == 1) {
        archetype.parentRowOffset = place(4);
        archetype.childBeginOffset = place(4);
        archetype.childEndOffset = place(4);
      } else {
        archetype.parentLocationOffset = place(scheme.locationBits / 8);
        bool followed = false;
        module.walk([&](QueryOp query) {
          followed |= query.getCascade() &&
                      query.getCascade().getAttr() == archetype.sortedBy &&
                      layout.cascadeFollowsEvents(query);
        });
        if (followed) {
          for (int64_t other = 0; other < others; ++other) {
            uint64_t begin = place(4);
            archetype.childRangeOffsets.push_back({begin, place(4)});
          }
          archetype.marksOffset = llvm::alignTo(end, kColumnAlignment);
          end = archetype.marksOffset + 8 * archetype.markWords();
          layout.zeroed.push_back(
              {archetype.marksOffset, uint64_t(8 * archetype.markWords())});
        }
      }
      // A depth for every entity that can have a parent, and two more.
      archetype.levelCapacity = sources + 2;
      uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
      archetype.levelStartOffset = offset;
      end = offset + 4 * archetype.levelCapacity;
      archetype.newRowOffset = place(4);
      for (const WorldColumn &column : archetype.columns)
        archetype.columnScratchOffsets.push_back(
            place(storageBytes(column.type)));
      archetype.idScratchOffset = place(scheme.idBits / 8);
    }
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
                                 place(storageBytes(type))});
        }
      }
    }
  }

  // A target id and a value per row, for each apply and each archetype its
  // query matches.
  // Accumulates into resources use the same buffers, the id saying only
  // whether the row sent a value.
  module.walk([&](Operation *apply) {
    if (!isa<ApplyOp, AccumulateOp>(apply))
      return;
    WorldApply entry;
    entry.type = apply->getOperand(apply->getNumOperands() - 1).getType();
    for (ArchetypeOp source :
         getMatchedArchetypes(apply->getParentOfType<QueryOp>())) {
      for (WorldArchetype &archetype : layout.archetypes) {
        if (archetype.op != source)
          continue;
        auto place = [&](uint64_t bytes) {
          uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
          end = offset + bytes * archetype.capacity;
          return offset;
        };
        uint64_t ids = place(scheme.idBits / 8);
        entry.buffers.push_back(
            {archetype.index, ids, place(storageBytes(entry.type))});
      }
    }
    if (auto edges = apply->getParentOfType<EdgesOp>()) {
      // A slot per edge: of the table, or of a linked tree's keys.
      auto relation =
          symbols.lookup<RelationOp>(edges.getRelationAttr().getAttr());
      entry.edgeCapacity = relation.getTree() && !relation.getSorted()
                               ? layout.entityKeys
                               : relation.getCapacity();
      auto place = [&](uint64_t bytes) {
        uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
        end = offset + bytes * entry.edgeCapacity;
        return offset;
      };
      entry.edgeIdOffset = place(scheme.idBits / 8);
      entry.edgeValueOffset = place(storageBytes(entry.type));
    }
    layout.applies.push_back(std::move(entry));
  });

  // Relations: the edge table, the offsets, and the scratch for sorting.
  // When each entity was connected, for a tree some trigger is up.
  for (WorldRelation &relation : layout.relations)
    if (llvm::is_contained(
            layout.stamps.getStamps(),
            Stamp{Trigger::Connected, RelationOp(relation.op).getSymNameAttr(),
                  StringAttr::get(module.getContext(), "")})) {
      relation.connectedOffset = llvm::alignTo(end, kColumnAlignment);
      end = relation.connectedOffset + 8 * layout.entityKeys;
      layout.zeroed.push_back(
          {relation.connectedOffset, uint64_t(8 * layout.entityKeys)});
    }
  // The marks of a tree that some reactive query follows events down.
  auto placeMarks = [&](WorldRelation &relation, int64_t bits) {
    bool followed = false;
    module.walk([&](QueryOp query) {
      followed |= query.getCascade() &&
                  query.getCascade().getAttr() ==
                      RelationOp(relation.op).getSymNameAttr() &&
                  layout.cascadeFollowsEvents(query);
    });
    if (!followed)
      return;
    relation.markBits = bits;
    relation.marksOffset = llvm::alignTo(end, kColumnAlignment);
    end = relation.marksOffset + 8 * relation.markWords();
    layout.zeroed.push_back(
        {relation.marksOffset, uint64_t(8 * relation.markWords())});
  };
  for (WorldRelation &relation : layout.relations) {
    RelationOp op = relation.op;
    StringAttr name = op.getSymNameAttr();
    bool hasIn = false, hasOut = false, hasDead = false;
    module.walk([&](EdgesOp edges) {
      if (edges.getRelationAttr().getAttr() != name)
        return;
      hasIn |= !edges.isOut();
      hasOut |= edges.isOut();
    });
    // A tree is read from child to parent (`up`) and, to order it, from
    // parent to children.
    if (relation.tree)
      hasIn = hasOut = true;
    relation.byTarget = hasIn && !hasOut;
    module.walk([&](DisconnectOp disconnect) {
      hasDead |= disconnect->getParentOfType<EdgesOp>()
                     .getRelationAttr()
                     .getAttr() == name;
    });
    relation.offsetBits = relation.capacity < (int64_t(1) << 31) &&
                                  layout.entityKeys < (int64_t(1) << 31)
                              ? 32
                              : 64;
    relation.slots = relation.capacity;
    // A tree that is not sorted: a slot per entity key, linked.
    if (relation.linked) {
      int64_t keys = layout.entityKeys;
      relation.slots = keys;
      auto perKey = [&](uint64_t bytes) {
        uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
        end = offset + bytes * keys;
        return offset;
      };
      uint64_t idBytes = scheme.idBits / 8, link = relation.offsetBits / 8;
      relation.sourceOffset = perKey(idBytes);
      relation.targetOffset = perKey(idBytes);
      for (auto [fieldName, typeAttr] :
           llvm::zip(op.getFieldNames(), op.getFieldTypes())) {
        Type type = cast<TypeAttr>(typeAttr).getValue();
        uint64_t bytes = storageBytes(type);
        if (bytes == 0)
          return op.emitOpError("field ")
                 << fieldName << " has type " << type
                 << ", which world storage does not support";
        relation.fields.push_back(
            {name, cast<StringAttr>(fieldName), type, perKey(bytes)});
      }
      if (hasDead)
        relation.deadOffset = perKey(1);
      relation.firstChildOffset = perKey(link);
      relation.lastChildOffset = perKey(link);
      relation.childCountOffset = perKey(link);
      relation.nextSiblingOffset = perKey(link);
      relation.previousSiblingOffset = perKey(link);
      relation.positionOffset = perKey(link);
      // A slot's owner says whether it holds an edge, 0 for none, from the
      // start. (The links are made when the tree is first built, which is
      // before anything follows them: a new world's trees are unclean.)
      layout.zeroed.push_back({relation.sourceOffset, idBytes * keys});
      relation.orderCapacity = 2 * relation.capacity;
      placeMarks(relation, relation.orderCapacity);
      auto perEdge = [&](uint64_t bytes) {
        uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
        end = offset + bytes * relation.orderCapacity;
        return offset;
      };
      relation.orderOffset = perEdge(idBytes);
      relation.orderParentOffset = perEdge(idBytes);
      if (scheme.hasIds()) {
        relation.orderLocationOffset = perEdge(scheme.locationBits / 8);
        relation.orderParentLocationOffset =
            perEdge(scheme.locationBits / 8);
      }
      continue;
    }
    auto place = [&](uint64_t bytes, int64_t elements) {
      uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
      end = offset + bytes * elements;
      return offset;
    };
    int64_t edges = relation.capacity;
    int64_t keys = layout.entityKeys;
    uint64_t idBytes = scheme.idBits / 8, offsetBytes = relation.offsetBits / 8;
    relation.sourceOffset = place(idBytes, edges);
    relation.targetOffset = place(idBytes, edges);
    for (auto [fieldName, typeAttr] :
         llvm::zip(op.getFieldNames(), op.getFieldTypes())) {
      Type type = cast<TypeAttr>(typeAttr).getValue();
      uint64_t bytes = storageBytes(type);
      if (bytes == 0)
        return op.emitOpError("field ")
               << fieldName << " has type " << type
               << ", which world storage does not support";
      relation.fields.push_back(
          {name, cast<StringAttr>(fieldName), type, place(bytes, edges)});
    }
    if (hasDead)
      relation.deadOffset = place(1, edges);
    relation.sortedOffset = place(offsetBytes, keys + 1);
    if (hasIn && hasOut) {
      relation.indexOffset = place(offsetBytes, keys + 1);
      relation.indexEdgesOffset = place(offsetBytes, edges);
    }
    relation.cursorOffset = place(offsetBytes, keys);
    relation.sourceScratchOffset = place(idBytes, edges);
    relation.targetScratchOffset = place(idBytes, edges);
    for (const WorldColumn &field : relation.fields)
      relation.fieldScratchOffsets.push_back(
          place(storageBytes(field.type), edges));
    if (relation.tree) {
      relation.orderCapacity = edges;
      relation.orderOffset = place(idBytes, edges);
      relation.orderParentOffset = place(idBytes, edges);
      if (relation.sortedArchetype() >= 0)
        placeMarks(relation,
                   layout.archetypes[relation.sortedArchetype()].capacity);
    }
  }

  // Edges connected inside queries: per row a source, a target and the
  // values, for each archetype the query matches.
  module.walk([&](ConnectOp connect) {
    auto query = connect->getParentOfType<QueryOp>();
    if (!query)
      return;
    auto relation = symbols.lookup<RelationOp>(
        connect.getRelationAttr().getAttr());
    WorldConnect entry;
    for (ArchetypeOp source : getMatchedArchetypes(query)) {
      for (WorldArchetype &archetype : layout.archetypes) {
        if (archetype.op != source)
          continue;
        auto place = [&](uint64_t bytes) {
          uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
          end = offset + bytes * archetype.capacity;
          return offset;
        };
        WorldConnect::Buffer buffer;
        buffer.archetype = archetype.index;
        buffer.sourceOffset = place(scheme.idBits / 8);
        buffer.targetOffset = place(scheme.idBits / 8);
        for (Attribute typeAttr : relation.getFieldTypes())
          buffer.valueOffsets.push_back(
              place(storageBytes(cast<TypeAttr>(typeAttr).getValue())));
        entry.buffers.push_back(std::move(buffer));
      }
    }
    layout.connects.push_back(std::move(entry));
  });

  // The rings of the event logs.
  for (WorldLog &log : layout.logs) {
    if (!log.exists())
      continue;
    auto place = [&](uint64_t bytes) {
      uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
      end = offset + bytes * log.capacity;
      return offset;
    };
    log.idsOffset = place(scheme.idBits / 8);
    log.ticksOffset = place(8);
  }

  // The entity table.
  auto placeTable = [&](uint64_t bytes) {
    uint64_t offset = llvm::alignTo(end, kColumnAlignment) + kStagger;
    end = offset + bytes * layout.entityCapacity;
    return offset;
  };
  if (scheme.hasGenerations())
    layout.generationOffset = placeTable(scheme.generationStorageBits / 8);
  if (scheme.hasIds())
    layout.locationOffset = placeTable(scheme.locationBits / 8);
  layout.totalBytes = llvm::alignTo(end, kArenaAlignment);
  return layout;
}
