#ifndef ECS_TYPES
#define ECS_TYPES

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/BuiltinAttributes.td"
include "Ecs/EcsDialect.td"

class Ecs_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Ecs_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def Ecs_RefType : Ecs_Type<"Ref", "ref"> {
  let summary = "Reference to one component of the entity a query visits";
  let description = [{
    `!ecs.ref<@Position>` grants read access, `!ecs.ref<@Position, mut>`
    grants read and write access. A ref only exists as a block argument of
    an `ecs.query`, so it can never outlive the iteration step.
  }];
  let parameters = (ins "FlatSymbolRefAttr":$component, "bool":$isMutable);
  let hasCustomAssemblyFormat = 1;
}

def Ecs_EntityType : Ecs_Type<"Entity", "entity"> {
  let summary = "The id of an entity";
  let description = [{
    A stable id: it stays valid while the entity moves between rows and
    archetypes and stops being alive when the entity is despawned. A
    component or resource field of this type is a relation. How many bits
    an id takes is decided by the world layout, not by the program.
  }];
}

#endif // ECS_TYPES
