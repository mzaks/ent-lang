#include "Ent/Access.h"
#include "Ent/Structure.h"

#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;
using namespace mlir::ent;

std::string mlir::ent::formatColumn(const Column &column) {
  auto [archetype, component, field] = column;
  if (!archetype && component.getValue().empty())
    return field.getValue().empty() ? "entities" : field.getValue().str();
  StringRef name = field.getValue();
  if (archetype && !component.getValue().empty() &&
      (name == "+" || name == "-" || name == "@"))
    return (archetype.getValue() + "." + component.getValue() + name).str();
  if (!archetype)
    return (component.getValue() + "." + field.getValue()).str();
  if (component.getValue().empty())
    return (archetype.getValue() + (field.getValue().empty() ? ".count" : ".id"))
        .str();
  if (field.getValue().empty())
    return (archetype.getValue() + "." + component.getValue() + "?").str();
  return (archetype.getValue() + "." + component.getValue() + "." +
          field.getValue())
      .str();
}

StringAttr mlir::ent::getStampColumnField(MLIRContext *context,
                                          const Stamp &stamp) {
  switch (stamp.kind) {
  case Trigger::Added:
    return StringAttr::get(context, "+");
  case Trigger::Removed:
    return StringAttr::get(context, "-");
  case Trigger::Changed:
    return StringAttr::get(context, stamp.field.getValue() + "@");
  }
  llvm_unreachable("unknown stamp kind");
}

bool mlir::ent::hasOwnEffects(Operation *op) {
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

SystemAccess mlir::ent::computeAccess(SystemOp system,
                                      ArrayRef<ArchetypeOp> archetypes) {
  SystemAccess access;
  llvm::DenseMap<Operation *, SmallVector<ArchetypeOp>> matched;

  auto matchedArchetypes = [&](QueryOp query) -> ArrayRef<ArchetypeOp> {
    auto [it, inserted] = matched.try_emplace(query);
    if (inserted) {
      for (ArchetypeOp archetype : archetypes) {
        bool all = llvm::all_of(
            query.getBody().getArgumentTypes(), [&](Type type) {
              return archetype.contains(cast<RefType>(type).getComponent());
            });
        if (all)
          it->second.push_back(archetype);
      }
    }
    return it->second;
  };

  // The presence of an optional component is a column with an empty field
  // name; an archetype's entity count one with an empty component name.
  StringAttr empty = StringAttr::get(system.getContext(), "");
  StringAttr presence = empty;

  // A spawn or despawn changes which entities an archetype holds: it
  // writes the count and, since rows are appended or moved, every column.
  StringAttr idField = StringAttr::get(system.getContext(), "id");
  // The entity table, which maps ids to archetypes and rows: written by
  // every structural change, read by every lookup.
  Column entityTable{StringAttr(), empty, empty};
  // Stamps some reactive query observes; ops that cause those events write
  // them and read the tick counter, reactive queries read them.
  MLIRContext *context = system.getContext();
  StampPlan stamps = StampPlan::compute(system->getParentOfType<ModuleOp>());
  Column ticks{StringAttr(), empty, StringAttr::get(context, "ticks")};
  auto writeStamp = [&](ArchetypeOp archetype, const Stamp &stamp) {
    if (!stamps.stores(archetype, stamp))
      return;
    access.writes.insert({archetype.getSymNameAttr(), stamp.component,
                          getStampColumnField(context, stamp)});
    access.reads.insert(ticks);
  };
  // A write to `field` of `component` (every field if `field` is null).
  auto writeChanged = [&](ArchetypeOp archetype, StringAttr component,
                          StringAttr field) {
    writeStamp(archetype, {Trigger::Changed, component, empty});
    if (field) {
      writeStamp(archetype, {Trigger::Changed, component, field});
      return;
    }
    auto componentOp = SymbolTable::lookupNearestSymbolFrom<ComponentOp>(
        system, FlatSymbolRefAttr::get(component));
    for (Attribute name : componentOp.getFieldNames())
      writeStamp(archetype,
                 {Trigger::Changed, component, cast<StringAttr>(name)});
  };

  auto writeStructure = [&](ArchetypeOp archetype) {
    // Rows move with their stamps; a spawn or move stamps its row.
    for (const Stamp &stamp : stamps.getStamps())
      writeStamp(archetype, stamp);
    access.writes.insert(entityTable);
    StringAttr name = archetype.getSymNameAttr();
    access.writes.insert({name, empty, empty});
    access.writes.insert({name, empty, idField});
    for (Attribute attr : archetype.getComponents()) {
      auto component = cast<FlatSymbolRefAttr>(attr);
      auto componentOp =
          SymbolTable::lookupNearestSymbolFrom<ComponentOp>(system, component);
      for (Attribute field : componentOp.getFieldNames())
        access.writes.insert(
            {name, component.getAttr(), cast<StringAttr>(field)});
      if (archetype.isOptional(component))
        access.writes.insert({name, component.getAttr(), presence});
    }
  };

  auto record = [&](Operation *op, Value ref, StringAttr field,
                    llvm::SetVector<Column> &into) {
    auto query = op->getParentOfType<QueryOp>();
    StringAttr component = cast<RefType>(ref.getType()).getComponent().getAttr();
    for (ArchetypeOp archetype : matchedArchetypes(query))
      into.insert({archetype.getSymNameAttr(), component, field});
  };

  system.getBody().walk([&](Operation *op) {
    if (auto get = dyn_cast<GetOp>(op))
      return record(op, get.getRef(), get.getFieldAttr(), access.reads);
    if (auto set = dyn_cast<SetOp>(op)) {
      StringAttr component =
          cast<RefType>(set.getRef().getType()).getComponent().getAttr();
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>()))
        writeChanged(archetype, component, set.getFieldAttr());
      return record(op, set.getRef(), set.getFieldAttr(), access.writes);
    }
    if (auto read = dyn_cast<ReadOp>(op))
      return (void)access.reads.insert(
          {StringAttr(), read.getResourceAttr().getAttr(), read.getFieldAttr()});
    if (auto accumulate = dyn_cast<AccumulateOp>(op))
      return (void)access.writes.insert({StringAttr(),
                                         accumulate.getResourceAttr().getAttr(),
                                         accumulate.getFieldAttr()});
    if (auto write = dyn_cast<WriteOp>(op))
      return (void)access.writes.insert({StringAttr(),
                                         write.getResourceAttr().getAttr(),
                                         write.getFieldAttr()});
    if (isa<AddOp, RemoveOp>(op)) {
      // What the change writes depends on how each matched archetype
      // stores the component.
      bool add = isa<AddOp>(op);
      auto component = cast<FlatSymbolRefAttr>(op->getAttr("component"));
      auto componentOp = SymbolTable::lookupNearestSymbolFrom<ComponentOp>(
          system, component);
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>())) {
        StringAttr name = archetype.getSymNameAttr();
        ComponentChange change = classifyChange(archetype, component, add);
        if (change.kind == ComponentChange::Move) {
          writeStructure(archetype);
          writeStructure(change.target);
          continue;
        }
        if (change.kind == ComponentChange::Presence) {
          access.writes.insert({name, component.getAttr(), presence});
          writeStamp(archetype, {add ? Trigger::Added : Trigger::Removed,
                                 component.getAttr(), empty});
        }
        if (add && change.kind != ComponentChange::NoTarget)
          writeChanged(archetype, component.getAttr(), StringAttr());
        if (add && change.kind != ComponentChange::NoTarget)
          for (Attribute field : componentOp.getFieldNames())
            access.writes.insert(
                {name, component.getAttr(), cast<StringAttr>(field)});
      }
      return;
    }
    if (auto lookup = dyn_cast<LookupOp>(op)) {
      // The entity may live in any archetype holding the component.
      access.reads.insert(entityTable);
      FlatSymbolRefAttr component = lookup.getComponentAttr();
      for (ArchetypeOp archetype : archetypes) {
        if (!archetype.contains(component))
          continue;
        StringAttr name = archetype.getSymNameAttr();
        access.reads.insert({name, component.getAttr(), lookup.getFieldAttr()});
        if (archetype.isOptional(component))
          access.reads.insert({name, component.getAttr(), presence});
      }
      return;
    }
    if (auto apply = dyn_cast<ApplyOp>(op)) {
      // Like a lookup, but the field is combined into: written (which
      // conflicts like a read too) in every archetype holding it.
      access.reads.insert(entityTable);
      FlatSymbolRefAttr component = apply.getComponentAttr();
      for (ArchetypeOp archetype : archetypes) {
        if (!archetype.contains(component))
          continue;
        StringAttr name = archetype.getSymNameAttr();
        access.writes.insert({name, component.getAttr(), apply.getFieldAttr()});
        writeChanged(archetype, component.getAttr(), apply.getFieldAttr());
        if (archetype.isOptional(component))
          access.reads.insert({name, component.getAttr(), presence});
      }
      return;
    }
    if (isa<EntityOp>(op)) {
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>()))
        access.reads.insert({archetype.getSymNameAttr(), empty, idField});
      return;
    }
    if (auto spawn = dyn_cast<SpawnOp>(op)) {
      for (ArchetypeOp archetype : archetypes)
        if (archetype.getSymNameAttr() == spawn.getArchetypeAttr().getAttr())
          writeStructure(archetype);
      return;
    }
    if (isa<DespawnOp>(op)) {
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>()))
        writeStructure(archetype);
      return;
    }
    if (auto query = dyn_cast<QueryOp>(op)) {
      // A query iterates its archetypes' entities, so it reads their
      // counts.
      for (ArchetypeOp archetype : matchedArchetypes(query))
        access.reads.insert({archetype.getSymNameAttr(), empty, empty});
      // A reactive query reads the stamps of its triggers and advances the
      // tick counter.
      SmallVector<Trigger> triggers = getTriggers(query);
      if (!triggers.empty())
        access.writes.insert(ticks);
      for (const Trigger &trigger : triggers)
        for (ArchetypeOp archetype : matchedArchetypes(query))
          if (stamps.stores(archetype, getStamp(trigger)))
            access.reads.insert({archetype.getSymNameAttr(),
                                 trigger.component.getAttr(),
                                 getStampColumnField(context,
                                                     getStamp(trigger))});
      // A query binding an optional component runs only where it is
      // present, so it reads the presence.
      for (Type type : query.getBody().getArgumentTypes()) {
        FlatSymbolRefAttr component = cast<RefType>(type).getComponent();
        for (ArchetypeOp archetype : matchedArchetypes(query))
          if (archetype.isOptional(component))
            access.reads.insert(
                {archetype.getSymNameAttr(), component.getAttr(), presence});
      }
      return;
    }
    if (isa<YieldOp>(op))
      return;
    if (!access.opaqueOp && hasOwnEffects(op))
      access.opaqueOp = op;
  });
  return access;
}
