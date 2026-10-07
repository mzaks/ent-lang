#include "Ent/Structure.h"

#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;
using namespace mlir::ent;

bool mlir::ent::canMatch(QueryOp query, function_ref<bool(Attribute)> holds,
                         function_ref<bool(Attribute)> always) {
  return llvm::all_of(query.getRequired(),
                      [&](FlatSymbolRefAttr c) { return holds(c); }) &&
         llvm::none_of(query.getWithout(),
                       [&](FlatSymbolRefAttr c) { return always(c); }) &&
         llvm::all_of(query.getAnyGroups(), [&](const auto &group) {
           return llvm::any_of(group,
                               [&](FlatSymbolRefAttr c) { return holds(c); });
         });
}

bool mlir::ent::matches(QueryOp query, ArchetypeOp archetype) {
  return canMatch(
      query,
      [&](Attribute c) {
        return archetype.contains(cast<FlatSymbolRefAttr>(c));
      },
      [&](Attribute c) {
        auto component = cast<FlatSymbolRefAttr>(c);
        return archetype.contains(component) &&
               !archetype.isOptional(component);
      });
}

SmallVector<FlatSymbolRefAttr> PresenceTest::components() const {
  SmallVector<FlatSymbolRefAttr> all(present);
  all.append(absent.begin(), absent.end());
  for (const auto &group : anyPresent)
    all.append(group.begin(), group.end());
  return all;
}

PresenceTest mlir::ent::getPresenceTest(QueryOp query,
                                        ArchetypeOp archetype) {
  PresenceTest test;
  for (FlatSymbolRefAttr component : query.getRequired())
    if (archetype.isOptional(component))
      test.present.push_back(component);
  for (FlatSymbolRefAttr component : query.getWithout())
    if (archetype.isOptional(component))
      test.absent.push_back(component);
  for (const auto &group : query.getAnyGroups()) {
    SmallVector<FlatSymbolRefAttr> optional;
    bool always = false;
    for (FlatSymbolRefAttr component : group) {
      if (archetype.isOptional(component))
        optional.push_back(component);
      else if (archetype.contains(component))
        always = true;
    }
    if (!always)
      test.anyPresent.push_back(std::move(optional));
  }
  return test;
}

SmallVector<ArchetypeOp> mlir::ent::getMatchedArchetypes(QueryOp query) {
  SmallVector<ArchetypeOp> matched;
  auto module = query->getParentOfType<ModuleOp>();
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>())
    if (matches(query, archetype))
      matched.push_back(archetype);
  return matched;
}

FlatSymbolRefAttr mlir::ent::getTrustedEndpoint(RelationOp relation,
                                                bool target) {
  FlatSymbolRefAttr component = relation.getEndpoint(target);
  if (!component)
    return {};
  auto module = relation->getParentOfType<ModuleOp>();
  // A tree that is not sorted loses the edges of and to an entity when
  // the entity is despawned, so its edges' ends are always alive.
  bool endsAlive = relation.getTree() && !relation.getSorted();
  WalkResult result = module.walk([&](Operation *op) {
    if (auto remove = dyn_cast<RemoveOp>(op))
      if (remove.getComponentAttr() == component)
        return WalkResult::interrupt();
    if (isa<DespawnOp>(op) && !endsAlive)
      for (ArchetypeOp archetype :
           getMatchedArchetypes(op->getParentOfType<QueryOp>()))
        if (archetype.contains(component))
          return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return result.wasInterrupted() ? FlatSymbolRefAttr() : component;
}

/// Whether `archetype` can hold a source or a target of `relation`, which
/// names what both have.
static bool holdsEnd(ArchetypeOp archetype, RelationOp relation) {
  return archetype.contains(relation.getFromAttr()) ||
         archetype.contains(relation.getToAttr());
}

RelationOp mlir::ent::getSortingTree(ArchetypeOp archetype) {
  auto module = archetype->getParentOfType<ModuleOp>();
  for (RelationOp relation : module.getOps<RelationOp>())
    if (relation.getSorted() && holdsEnd(archetype, relation))
      return relation;
  return {};
}

FailureOr<SmallVector<ArchetypeOp>>
mlir::ent::getSortedArchetypes(RelationOp relation) {
  auto module = relation->getParentOfType<ModuleOp>();
  SmallVector<ArchetypeOp> holders;
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>())
    if (holdsEnd(archetype, relation))
      holders.push_back(archetype);
  for (bool target : {false, true}) {
    FlatSymbolRefAttr component = relation.getEndpoint(target);
    RemoveOp removed;
    module.walk([&](RemoveOp remove) {
      if (remove.getComponentAttr() == component)
        removed = remove;
    });
    if (removed) {
      InFlightDiagnostic diag =
          relation.emitOpError("is 'sorted', but a system removes ")
          << component << ", which its " << (target ? "targets" : "sources")
          << " have";
      diag.attachNote(removed.getLoc()) << "removed here";
      return diag;
    }
  }
  for (RelationOp other : module.getOps<RelationOp>()) {
    if (other == relation || !other.getSorted())
      continue;
    for (ArchetypeOp archetype : holders)
      if (holdsEnd(archetype, other)) {
        InFlightDiagnostic diag =
            relation.emitOpError("is 'sorted', and so is @")
            << other.getSymName() << "; both have their entities in @"
            << archetype.getSymName() << ", whose rows have one order";
        diag.attachNote(other.getLoc()) << "the other tree";
        return diag;
      }
  }
  return holders;
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

ComponentChange mlir::ent::classifyChange(ArchetypeOp archetype,
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

SmallVector<Trigger> mlir::ent::getTriggers(QueryOp query) {
  SmallVector<Trigger> triggers;
  auto list = query->getAttrOfType<ArrayAttr>(QueryOp::kTriggersAttr);
  if (!list)
    return triggers;
  for (Attribute attr : list) {
    auto entry = cast<ArrayAttr>(attr);
    StringRef kind = cast<StringAttr>(entry[0]).getValue();
    // After the field: a tree (`up @R`) and a log capacity, either or
    // both.
    std::optional<int64_t> logCapacity;
    FlatSymbolRefAttr via;
    Trigger::Where where = Trigger::Own;
    for (Attribute extra : entry.getValue().drop_front(3)) {
      if (auto capacity = dyn_cast<IntegerAttr>(extra)) {
        logCapacity = capacity.getInt();
      } else if (auto direction = dyn_cast<StringAttr>(extra)) {
        where = direction.getValue() == "down" ? Trigger::Down
                                                : Trigger::Before;
      } else {
        via = cast<FlatSymbolRefAttr>(extra);
        if (where == Trigger::Own)
          where = Trigger::Up;
      }
    }
    triggers.push_back({kind == "added"     ? Trigger::Added
                        : kind == "removed" ? Trigger::Removed
                                            : Trigger::Changed,
                        cast<FlatSymbolRefAttr>(entry[1]),
                        cast<StringAttr>(entry[2]), logCapacity, via, where});
  }
  for (size_t named = triggers.size(), k = 0; k < named; ++k) {
    FlatSymbolRefAttr via = triggers[k].via;
    if (via && llvm::none_of(triggers, [&](const Trigger &other) {
          return other.kind == Trigger::Connected && other.component == via;
        }))
      triggers.push_back({Trigger::Connected, via,
                          StringAttr::get(query->getContext(), ""),
                          std::nullopt});
  }
  return triggers;
}

Stamp mlir::ent::getStamp(const Trigger &trigger) {
  return {trigger.kind, trigger.component.getAttr(), trigger.field};
}

/// Whether `stamp` means something in `archetype`.
static bool isMeaningful(ArchetypeOp archetype, const Stamp &stamp) {
  // (When an entity was connected is kept by the relation, per entity.)
  if (stamp.kind == Trigger::Connected)
    return false;
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
      // An ancestor's event is looked for wherever the component is.
      if (trigger.via) {
        for (ArchetypeOp archetype : module.getOps<ArchetypeOp>())
          if (archetype.contains(trigger.component))
            observers[index].push_back(archetype);
        continue;
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

LogicalResult mlir::ent::inferArchetypes(ModuleOp module) {
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

  // What queries add and remove, and the queries doing it.
  struct Change {
    QueryOp query;
    Attribute component;
    bool add;
  };
  SmallVector<Change> changes;
  module.walk([&](Operation *op) {
    if (!isa<AddOp, RemoveOp>(op))
      return;
    Change change;
    change.query = op->getParentOfType<QueryOp>();
    change.component = op->getAttr("component");
    change.add = isa<AddOp>(op);
    changes.push_back(std::move(change));
  });

  SymbolTable symbols(module);
  auto defaultCapacity =
      module->getAttrOfType<IntegerAttr>("ent.default_capacity");
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
          bool matches = canMatch(
              change.query,
              [&](Attribute component) {
                return base.contains(component) ||
                       optional.contains(component);
              },
              [&](Attribute component) {
                return base.contains(component) &&
                       !optional.contains(component);
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
            << "components a capacity, or set ent.default_capacity on the "
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
