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
  if (!archetype && field.getValue().empty())
    return (component.getValue() + ".edges").str();
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
  case Trigger::Connected:
    return StringAttr::get(context, "~");
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

void mlir::ent::addConditionReads(Region &condition, SystemAccess &access) {
  condition.walk([&](ReadOp read) {
    access.reads.insert(
        {StringAttr(), read.getResourceAttr().getAttr(), read.getFieldAttr()});
  });
}

/// The entity table, which maps ids to archetypes and rows: written by
/// every structural change, read by every lookup.
static Column entityTableColumn(MLIRContext *context) {
  StringAttr empty = StringAttr::get(context, "");
  return {StringAttr(), empty, empty};
}

/// Record that `stamp` is written for entities of `archetype`, if some
/// reactive query observes it there.
static void writeStamp(SystemAccess &access, const StampPlan &stamps,
                       ArchetypeOp archetype, const Stamp &stamp) {
  if (!stamps.stores(archetype, stamp))
    return;
  MLIRContext *context = archetype.getContext();
  access.writes.insert({archetype.getSymNameAttr(), stamp.component,
                        getStampColumnField(context, stamp)});
  access.reads.insert({StringAttr(), StringAttr::get(context, ""),
                       StringAttr::get(context, "ticks")});
}

/// A spawn or despawn changes which entities an archetype holds: it
/// writes the count and, since rows are appended or moved, every column.
static void writeStructure(SystemAccess &access, const StampPlan &stamps,
                           ArchetypeOp archetype) {
  MLIRContext *context = archetype.getContext();
  StringAttr empty = StringAttr::get(context, "");
  // Rows move with their stamps; a spawn or move stamps its row.
  for (const Stamp &stamp : stamps.getStamps())
    writeStamp(access, stamps, archetype, stamp);
  access.writes.insert(entityTableColumn(context));
  StringAttr name = archetype.getSymNameAttr();
  access.writes.insert({name, empty, empty});
  access.writes.insert({name, empty, StringAttr::get(context, "id")});
  for (Attribute attr : archetype.getComponents()) {
    auto component = cast<FlatSymbolRefAttr>(attr);
    auto componentOp =
        SymbolTable::lookupNearestSymbolFrom<ComponentOp>(archetype, component);
    for (Attribute field : componentOp.getFieldNames())
      access.writes.insert({name, component.getAttr(), cast<StringAttr>(field)});
    if (archetype.isOptional(component))
      access.writes.insert({name, component.getAttr(), empty});
  }
}

SystemAccess mlir::ent::computeAccess(ExternOp external,
                                      ArrayRef<ArchetypeOp> archetypes) {
  SystemAccess access;
  if (!external.hasContract()) {
    access.opaqueOp = external;
    return access;
  }
  MLIRContext *context = external.getContext();
  StringAttr empty = StringAttr::get(context, "");
  StampPlan stamps = StampPlan::compute(external->getParentOfType<ModuleOp>());

  // Everything the header gives access to for a name: every field, in
  // every archetype holding the component, with the rows' ids and count.
  auto add = [&](FlatSymbolRefAttr ref, bool write) {
    llvm::SetVector<Column> &into = write ? access.writes : access.reads;
    Operation *target = SymbolTable::lookupNearestSymbolFrom(external, ref);
    if (auto archetype = dyn_cast<ArchetypeOp>(target))
      return writeStructure(access, stamps, archetype);
    if (auto resource = dyn_cast<ResourceOp>(target)) {
      for (Attribute field : resource.getFieldNames())
        into.insert({StringAttr(), ref.getAttr(), cast<StringAttr>(field)});
      return;
    }
    access.reads.insert(entityTableColumn(context));
    if (auto relation = dyn_cast<RelationOp>(target)) {
      into.insert({StringAttr(), ref.getAttr(), empty});
      for (Attribute field : relation.getFieldNames())
        into.insert({StringAttr(), ref.getAttr(), cast<StringAttr>(field)});
      return;
    }
    auto component = cast<ComponentOp>(target);
    for (ArchetypeOp archetype : archetypes) {
      if (!archetype.contains(ref))
        continue;
      StringAttr name = archetype.getSymNameAttr();
      access.reads.insert({name, empty, empty});
      access.reads.insert({name, empty, StringAttr::get(context, "id")});
      for (Attribute field : component.getFieldNames())
        into.insert({name, ref.getAttr(), cast<StringAttr>(field)});
      if (archetype.isOptional(ref))
        into.insert({name, ref.getAttr(), empty});
    }
  };
  if (ArrayAttr reads = external.getReadsAttr())
    for (Attribute attr : reads)
      add(cast<FlatSymbolRefAttr>(attr), /*write=*/false);
  if (ArrayAttr writes = external.getWritesAttr())
    for (Attribute attr : writes)
      add(cast<FlatSymbolRefAttr>(attr), /*write=*/true);
  return access;
}

SystemAccess mlir::ent::computeAccess(Operation *system,
                                      ArrayRef<ArchetypeOp> archetypes) {
  if (auto external = dyn_cast<ExternOp>(system))
    return computeAccess(external, archetypes);
  return computeAccess(cast<SystemOp>(system), archetypes);
}

SystemAccess mlir::ent::computeAccess(SystemOp system,
                                      ArrayRef<ArchetypeOp> archetypes) {
  SystemAccess access;
  llvm::DenseMap<Operation *, SmallVector<ArchetypeOp>> matched;

  auto matchedArchetypes = [&](QueryOp query) -> ArrayRef<ArchetypeOp> {
    auto [it, inserted] = matched.try_emplace(query);
    if (inserted) {
      for (ArchetypeOp archetype : archetypes)
        if (matches(query, archetype))
          it->second.push_back(archetype);
    }
    return it->second;
  };

  // The presence of an optional component is a column with an empty field
  // name; an archetype's entity count one with an empty component name.
  StringAttr empty = StringAttr::get(system.getContext(), "");
  StringAttr presence = empty;

  StringAttr idField = StringAttr::get(system.getContext(), "id");
  Column entityTable = entityTableColumn(system.getContext());
  // Stamps some reactive query observes; ops that cause those events write
  // them and read the tick counter, reactive queries read them.
  MLIRContext *context = system.getContext();
  StampPlan stamps = StampPlan::compute(system->getParentOfType<ModuleOp>());
  Column ticks{StringAttr(), empty, StringAttr::get(context, "ticks")};
  auto writeStamp = [&](ArchetypeOp archetype, const Stamp &stamp) {
    ::writeStamp(access, stamps, archetype, stamp);
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
    ::writeStructure(access, stamps, archetype);
  };

  // A relation's fields are columns like a resource's, its sorted edges
  // and offsets one more (empty field, printed `R.edges`).
  auto relationOf = [&](Value ref) -> RelationOp {
    return SymbolTable::lookupNearestSymbolFrom<RelationOp>(
        system, cast<RefType>(ref.getType()).getComponent());
  };
  auto relationColumns = [&](StringAttr relation) {
    auto op = SymbolTable::lookupNearestSymbolFrom<RelationOp>(
        system, FlatSymbolRefAttr::get(relation));
    SmallVector<Column> columns{{StringAttr(), relation, empty}};
    for (Attribute field : op.getFieldNames())
      columns.push_back({StringAttr(), relation, cast<StringAttr>(field)});
    return columns;
  };
  // Following a tree from the visited entities: their ids, the sorted
  // edges, and the entities the edges lead to.
  auto followTree = [&](QueryOp query, FlatSymbolRefAttr relation) {
    access.reads.insert({StringAttr(), relation.getAttr(), empty});
    access.reads.insert(entityTable);
    for (ArchetypeOp archetype : matchedArchetypes(query))
      access.reads.insert({archetype.getSymNameAttr(), empty, idField});
  };
  auto record = [&](Operation *op, Value ref, StringAttr field,
                    llvm::SetVector<Column> &into) {
    if (RelationOp relation = relationOf(ref)) {
      into.insert({StringAttr(), relation.getSymNameAttr(), field});
      return;
    }
    auto refType = cast<RefType>(ref.getType());
    // Of the entities an `ent.each` runs for: in the archetypes those
    // may be in.
    if (auto arg = dyn_cast<BlockArgument>(ref))
      if (auto each = dyn_cast<EachOp>(arg.getOwner()->getParentOp())) {
        for (ArchetypeOp archetype : archetypes)
          if (matches(each, archetype))
            into.insert({archetype.getSymNameAttr(),
                         refType.getComponent().getAttr(), field});
        return;
      }
    auto query = op->getParentOfType<QueryOp>();
    if (refType.isUp()) {
      // An ancestor's: like a lookup, in any archetype holding the
      // component.
      for (ArchetypeOp archetype : archetypes)
        if (archetype.contains(refType.getComponent()))
          into.insert({archetype.getSymNameAttr(),
                       refType.getComponent().getAttr(), field});
      return;
    }
    StringAttr component = cast<RefType>(ref.getType()).getComponent().getAttr();
    // (An optional ref's, only where the archetype has the component.)
    for (ArchetypeOp archetype : matchedArchetypes(query))
      if (archetype.contains(refType.getComponent()))
        into.insert({archetype.getSymNameAttr(), component, field});
  };

  // The trees that keep a slot for the entities of `archetype` (those
  // that are not sorted, and whose ends it can hold) are written where
  // its entities go or its rows move.
  auto touchesTrees = [&](ArchetypeOp archetype) {
    for (RelationOp relation :
         system->getParentOfType<ModuleOp>().getOps<RelationOp>()) {
      if (!relation.getTree() || relation.getSorted())
        continue;
      bool holds = false;
      for (bool target : {false, true}) {
        FlatSymbolRefAttr component = relation.getEndpoint(target);
        holds |= !component || archetype.contains(component);
      }
      if (holds)
        for (const Column &column :
             relationColumns(relation.getSymNameAttr()))
          access.writes.insert(column);
    }
  };

  system.getBody().walk([&](Operation *op) {
    // How many entities each archetype has, which of them have what is
    // asked of each, and their ids where they are asked for.
    if (auto each = dyn_cast<EachOp>(op)) {
      for (ArchetypeOp archetype : archetypes) {
        if (!matches(each, archetype))
          continue;
        StringAttr name = archetype.getSymNameAttr();
        access.reads.insert({name, empty, empty});
        if (each.getEntity()) {
          access.reads.insert({name, empty, idField});
          access.reads.insert(entityTable);
        }
        for (auto list : {each.getRequired(), each.getExcluded()})
          for (FlatSymbolRefAttr component : list)
            if (archetype.isOptional(component))
              access.reads.insert({name, component.getAttr(), presence});
      }
      return;
    }
    if (auto get = dyn_cast<GetOp>(op))
      return record(op, get.getRef(), get.getFieldAttr(), access.reads);
    if (auto set = dyn_cast<SetOp>(op); set && relationOf(set.getRef()))
      return record(op, set.getRef(), set.getFieldAttr(), access.writes);
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
          // Rows move: a tree that keeps a slot for these entities notes
          // their locations again.
          touchesTrees(archetype);
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
    if (auto combine = dyn_cast<CombineOp>(op)) {
      // Into an ancestor's field: like an apply, in every archetype
      // holding the component. The query itself records what finding the
      // ancestor reads.
      FlatSymbolRefAttr component = combine.getRef().getType().getComponent();
      for (ArchetypeOp archetype : archetypes) {
        if (!archetype.contains(component))
          continue;
        access.writes.insert({archetype.getSymNameAttr(), component.getAttr(),
                              combine.getFieldAttr()});
        writeChanged(archetype, component.getAttr(), combine.getFieldAttr());
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
    // An archetype sorted by a tree is sorted again when it gains or
    // loses an entity, with the tree's edges.
    auto sortsTree = [&](ArchetypeOp archetype) {
      if (RelationOp tree = getSortingTree(archetype))
        for (const Column &column : relationColumns(tree.getSymNameAttr()))
          access.writes.insert(column);
    };
    if (auto spawn = dyn_cast<SpawnOp>(op)) {
      for (ArchetypeOp archetype : archetypes)
        if (archetype.getSymNameAttr() == spawn.getArchetypeAttr().getAttr()) {
          writeStructure(archetype);
          sortsTree(archetype);
        }
      return;
    }
    if (isa<DespawnOp>(op)) {
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>())) {
        writeStructure(archetype);
        sortsTree(archetype);
        // A tree that is not sorted drops the entity's edges with it.
        touchesTrees(archetype);
      }
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
      for (const Trigger &trigger : triggers) {
        // (An ancestor's stamp is wherever its component is.)
        ArrayRef<ArchetypeOp> observed =
            trigger.via ? archetypes : matchedArchetypes(query);
        for (ArchetypeOp archetype : observed)
          if (stamps.stores(archetype, getStamp(trigger)))
            access.reads.insert({archetype.getSymNameAttr(),
                                 trigger.component.getAttr(),
                                 getStampColumnField(context,
                                                     getStamp(trigger))});
      }
      // Where the query's bindings and filters are optional, it tests
      // their presence per entity, so it reads the presence.
      for (ArchetypeOp archetype : matchedArchetypes(query))
        for (FlatSymbolRefAttr component :
             getPresenceTest(query, archetype).components())
          access.reads.insert(
              {archetype.getSymNameAttr(), component.getAttr(), presence});
      // A cascading query follows its tree, and so does a ref to an
      // ancestor, which is the nearest one that has the component.
      if (FlatSymbolRefAttr cascade = query.getCascade())
        followTree(query, cascade);
      for (Type type : query.getBody().getArgumentTypes()) {
        auto refType = cast<RefType>(type);
        if (!refType.isUp())
          continue;
        followTree(query, refType.getVia());
        // (And every tree a path goes up, with what it looks for there.)
        if (refType.hasPath())
          for (Attribute attr : refType.getPath()) {
            auto step = cast<ArrayAttr>(attr);
            followTree(query, cast<FlatSymbolRefAttr>(step[1]));
            for (Attribute part : step.getValue().drop_front(2))
              for (ArchetypeOp archetype : archetypes)
                if (archetype.isOptional(cast<FlatSymbolRefAttr>(part)))
                  access.reads.insert(
                      {archetype.getSymNameAttr(),
                       cast<FlatSymbolRefAttr>(part).getAttr(), presence});
          }
        for (ArchetypeOp archetype : archetypes)
          if (archetype.isOptional(refType.getComponent()))
            access.reads.insert({archetype.getSymNameAttr(),
                                 refType.getComponent().getAttr(), presence});
      }
      return;
    }
    if (auto edges = dyn_cast<EdgesOp>(op)) {
      // Visits the sorted edges of each entity, found by its id.
      access.reads.insert(
          {StringAttr(), edges.getRelationAttr().getAttr(), empty});
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>()))
        access.reads.insert({archetype.getSymNameAttr(), empty, idField});
      return;
    }
    if (isa<ConnectOp, DisconnectOp>(op)) {
      // Adds or drops edges, and sorts them again: every column of the
      // relation moves. Sorting drops edges to entities no longer alive.
      StringAttr relation =
          isa<ConnectOp>(op)
              ? cast<ConnectOp>(op).getRelationAttr().getAttr()
              : op->getParentOfType<EdgesOp>().getRelationAttr().getAttr();
      for (const Column &column : relationColumns(relation))
        access.writes.insert(column);
      // The archetype sorted by the tree moves its rows with it.
      for (ArchetypeOp archetype : archetypes)
        if (RelationOp tree = getSortingTree(archetype);
            tree && tree.getSymNameAttr() == relation)
          writeStructure(archetype);
      access.reads.insert(entityTable);
      if (isa<ConnectOp>(op) && op->getParentOfType<QueryOp>())
        for (ArchetypeOp archetype :
             matchedArchetypes(op->getParentOfType<QueryOp>()))
          access.reads.insert({archetype.getSymNameAttr(), empty, empty});
      return;
    }
    if (isa<AppendOp, ClearOp, BufferLenOp, BufferAtOp, BufferOfOp>(op)) {
      // A buffer is one thing: its rows and how many they are.
      Column buffer{StringAttr(),
                    cast<FlatSymbolRefAttr>(op->getAttr("buffer")).getAttr(),
                    StringAttr::get(context, "rows")};
      if (isa<AppendOp, ClearOp>(op))
        access.writes.insert(buffer);
      else
        access.reads.insert(buffer);
      if (isa<AppendOp>(op) && op->getParentOfType<QueryOp>())
        for (ArchetypeOp archetype :
             matchedArchetypes(op->getParentOfType<QueryOp>()))
          access.reads.insert({archetype.getSymNameAttr(), empty, empty});
      return;
    }
    if (auto has = dyn_cast<HasOp>(op)) {
      FlatSymbolRefAttr component = has.getComponentAttr();
      for (ArchetypeOp archetype :
           matchedArchetypes(op->getParentOfType<QueryOp>()))
        if (archetype.isOptional(component))
          access.reads.insert(
              {archetype.getSymNameAttr(), component.getAttr(), presence});
      return;
    }
    if (isa<YieldOp>(op))
      return;
    if (!access.opaqueOp && hasOwnEffects(op))
      access.opaqueOp = op;
  });
  return access;
}
