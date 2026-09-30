#ifndef ECS_OPS
#define ECS_OPS

include "Ecs/EcsDialect.td"
include "Ecs/EcsTypes.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

// A symbol that declares an ordered list of scalar fields.
class Ecs_RecordOp<string mnemonic> : Ecs_Op<mnemonic, [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The type of field `name`, or null if there is no such field.
    ::mlir::Type getFieldType(::llvm::StringRef name);
  }];
}

def Ecs_ComponentOp : Ecs_RecordOp<"component"> {
  let summary = "Declare a component: an ordered list of scalar fields";
  let description = [{
    ```mlir
    ecs.component @Position (x: f32, y: f32)
    ```
  }];
}

def Ecs_ResourceOp : Ecs_RecordOp<"resource"> {
  let summary = "Declare a resource: world state that exists exactly once";
  let description = [{
    ```mlir
    ecs.resource @Clock (dt: f32, frame: i64)
    ```
    A resource is not an entity: no query visits it. Systems name it in
    their `reads` and `writes` like a component and access its fields with
    `ecs.read` and `ecs.write`. It lives in the world's storage next to the
    entity counts.
  }];
}

def Ecs_ArchetypeOp : Ecs_Op<"archetype", [
    Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Declare a set of components that entities are stored by";
  let description = [{
    ```mlir
    ecs.archetype @Body (@Position, @Velocity, @Mass) capacity 100000
    ```
    Every entity belongs to exactly one archetype, and each archetype is
    stored as its own table. The set of archetypes is closed and known at
    compile time, so every query is matched to its archetypes statically.

    `capacity` is a hard upper bound on the number of entities in the
    archetype. It lets the compiler lay out the whole world statically and
    decide from the size alone whether a loop over the archetype can ever
    be worth running in parallel.

    A component marked `optional` may be present or absent per entity:
    ```mlir
    ecs.archetype @Character (@Position, @Velocity, optional @Stunned)
        capacity 100000
    ```
    It is stored as its field columns plus a presence byte per entity, so
    gaining or losing it (`ecs.add`, `ecs.remove`) is a write to the
    entity's own row rather than a move to another archetype. Queries that
    bind it run only for the entities that currently have it.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       FlatSymbolRefArrayAttr:$components,
                       OptionalAttr<FlatSymbolRefArrayAttr>:$optional,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity);
  let assemblyFormat = [{
    $sym_name custom<ComponentList>($components, $optional)
    `capacity` $capacity attr-dict
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// True if the archetype stores `component`, always or optionally.
    bool contains(::mlir::FlatSymbolRefAttr component);
    /// True if `component` is optional in this archetype.
    bool isOptional(::mlir::FlatSymbolRefAttr component);
  }];
}

def Ecs_SystemOp : Ecs_Op<"system", [
    Symbol, IsolatedFromAbove, SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A system with declared component access";
  let description = [{
    ```mlir
    ecs.system @integrate(%dt: f32) reads [@Velocity] writes [@Position] {
      ecs.query (%p: !ecs.ref<@Position, mut>, %v: !ecs.ref<@Velocity>) {
        ...
      }
    }
    ```
    `writes` implies read access. `writes` may also name archetypes: the
    system changes their set of entities (`ecs.spawn`, `ecs.despawn`).
    Entry block arguments are the system's per-run parameters.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       SymbolRefArrayAttr:$reads,
                       SymbolRefArrayAttr:$writes);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    bool canRead(::mlir::FlatSymbolRefAttr component);
    bool canWrite(::mlir::FlatSymbolRefAttr component);
  }];
}

def Ecs_ScheduleOp : Ecs_Op<"schedule", [
    Symbol, IsolatedFromAbove, SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "An ordered list of systems that make up one frame";
  let description = [{
    ```mlir
    ecs.schedule @frame(%dt: f32) {
      ecs.run @integrate(%dt) : f32
    }
    ```
    Program order is the semantic order; a scheduler may reorder or
    parallelise runs whose access sets do not conflict.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Operations inside systems and schedules
//===----------------------------------------------------------------------===//

def Ecs_QueryOp : Ecs_Op<"query", [
    HasParent<"SystemOp">, SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run the body once for every entity that has all components";
  let description = [{
    Each entry block argument is an `!ecs.ref` to a distinct component, and
    refs are only used by `ecs.get` and `ecs.set`. Iteration order is
    unspecified, so the body must not depend on it.

    A reactive query lists triggers after its arguments:
    ```mlir
    ecs.query (%h: !ecs.ref<@Hull>, %b: !ecs.ref<@Bar, mut>)
        on [changed @Hull "hp", added @Hull] { ... }
    ```
    It runs only for the entities that had at least one of these events
    since the query last started, and that match the query now (an entity
    may have changed again, or moved, since): `added @C` when the entity
    gained `C` (by `ecs.add` or by being spawned with it), `removed @C`
    when it lost `C` while staying alive, `changed @C "f"` on every write
    to field `f` (`ecs.set`, `ecs.apply`, `ecs.add`, spawning), and
    `changed @C` on a write to any field of `C`. On its first run every
    existing entity counts as added and changed. Events the query causes
    itself count on its next run. The system must declare every trigger's
    component in `reads` or `writes`.
  }];
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The attribute holding a reactive query's triggers: an array of
    /// [kind, component, field] entries, the field empty unless the kind
    /// is "changed" with a field.
    static constexpr ::llvm::StringLiteral kTriggersAttr = "triggers";
  }];
}

def Ecs_GetOp : Ecs_Op<"get", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read one field through a component reference";
  let arguments = (ins Ecs_RefType:$ref, StrAttr:$field);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$ref $field attr-dict `:` type($ref) `->` type($result)";
}

def Ecs_SetOp : Ecs_Op<"set", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Write one field through a mutable component reference";
  let arguments = (ins Ecs_RefType:$ref, StrAttr:$field, AnyType:$value);
  let assemblyFormat =
      "$ref $field `,` $value attr-dict `:` type($ref) `,` type($value)";
  let hasVerifier = 1;
}

def Ecs_ReadOp : Ecs_Op<"read", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read one field of a resource";
  let description = [{
    ```mlir
    %dt = ecs.read @Clock "dt" : f32
    ```
    Allowed anywhere in a system, including inside queries: no query may
    write a resource, so every entity reads the same value. The system
    must declare the resource in `reads` or `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$resource $field attr-dict `:` type($result)";
  let hasVerifier = 1;
}

def Ecs_WriteOp : Ecs_Op<"write", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Write one field of a resource, outside queries";
  let description = [{
    ```mlir
    ecs.write @Clock "frame", %next : i64
    ```
    Allowed in a system but not inside a query: a write per entity would
    make every entity depend on the others, which rules out parallel and
    fused loops. The system must declare the resource in `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field,
                       AnyType:$value);
  let assemblyFormat =
      "$resource $field `,` $value attr-dict `:` type($value)";
  let hasVerifier = 1;
}

def Ecs_AddOp : Ecs_Op<"add", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Give the visited entity a component";
  let description = [{
    ```mlir
    ecs.add @Stunned(%seconds) : f32
    ```
    Gives the entity the enclosing query visits the component, with its
    fields initialised in declaration order. What that means depends on
    how the entity's archetype stores the component, not on the program:
    - optional in the archetype: set its presence (and fields);
    - always held by the archetype: overwrite the fields;
    - not held: move the entity to the archetype with exactly its
      components plus this one, which must exist. Moves are deferred to
      the end of the query, like despawns.
    The system must declare the component in `writes`; a move needs no
    archetypes declared, since whether a change moves entities is a storage
    decision and the analysis derives its effect from the storage.
  }];
  let arguments = (ins FlatSymbolRefAttr:$component,
                       Variadic<AnyType>:$values);
  let assemblyFormat =
      "$component `(` $values `)` attr-dict (`:` type($values)^)?";
  let hasVerifier = 1;
}

def Ecs_RemoveOp : Ecs_Op<"remove", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Take a component away from the visited entity";
  let description = [{
    ```mlir
    ecs.remove @Stunned
    ```
    Takes the component away from the entity the enclosing query visits:
    clears its presence where it is optional, moves the entity to the
    archetype with exactly its components minus this one where it is not
    (deferred to the end of the query), and does nothing where the
    archetype does not hold it. The same rules as for `ecs.add` apply.
  }];
  let arguments = (ins FlatSymbolRefAttr:$component);
  let assemblyFormat = "$component attr-dict";
  let hasVerifier = 1;
}

def Ecs_SpawnOp : Ecs_Op<"spawn", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Create an entity in an archetype and return its id";
  let description = [{
    ```mlir
    ecs.spawn @Bullet(%x, %y, %dx, %dy, %seconds) : f32, f32, f32, f32, f32
    ```
    Appends an entity to the archetype. The values initialise every field
    of the archetype's non-optional components, in the order of the
    components and then of their fields; optional components start absent.
    Spawning beyond the archetype's capacity is an error at run time. The
    system must declare the archetype in `writes`.

    The result is the entity's id, which stays valid while the entity moves
    between rows and archetypes and becomes invalid when it is despawned.
  }];
  let arguments = (ins FlatSymbolRefAttr:$archetype,
                       Variadic<AnyType>:$values);
  let results = (outs Ecs_EntityType:$entity);
  let assemblyFormat =
      "$archetype `(` $values `)` attr-dict (`:` type($values)^)?";
  let hasVerifier = 1;
}

def Ecs_EntityOp : Ecs_Op<"entity"> {
  let summary = "The id of the entity the enclosing query visits";
  let description = [{
    ```mlir
    %id = ecs.entity
    ```
    The same id `ecs.spawn` returned for the entity.
  }];
  let results = (outs Ecs_EntityType:$entity);
  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

def Ecs_LookupOp : Ecs_Op<"lookup", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read a field of another entity, by id";
  let description = [{
    ```mlir
    %x, %found = ecs.lookup %target @Position "x" : f32
    ```
    Reads field `x` of `@Position` of the entity with id `%target`, in
    whatever archetype it lives. `%found` is false, and the value 0, if
    the id is not alive or the entity does not have the component (its
    archetype does not hold it, or holds it optionally and it is absent).
    A component field of type `!ecs.entity` is how entities refer to each
    other.

    The system must declare the component in `reads` or `writes`. A query
    may not look up a field it writes itself: which entities would see the
    old value and which the new would depend on iteration order. Lookups
    only read; a system with lookups is not fused with others, for the same
    reason.
  }];
  let arguments = (ins Ecs_EntityType:$entity, FlatSymbolRefAttr:$component,
                       StrAttr:$field);
  let results = (outs AnyType:$value, I1:$found);
  let assemblyFormat = [{
    $entity $component $field attr-dict `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ecs_ApplyOp : Ecs_Op<"apply", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Combine a value into a field of another entity, by id";
  let description = [{
    ```mlir
    ecs.apply %target @Hull "hp" add %damage : f32
    ```
    Combines `%damage` into field `hp` of `@Hull` of the entity with id
    `%target`, with the rule `add`, `min` or `max` (signed for integers).
    Applies are deferred to the end of the enclosing query: then every
    target's field becomes `field ⊕ v1 ⊕ v2 ⊕ ...` over the values sent to
    it, before the query's despawns and moves take effect. So the query
    itself sees every target's old value and may read the field (with
    `ecs.get` or `ecs.lookup`) or set it; applies land on top.

    The values are combined in a fixed order (by apply, then archetype,
    then row), so the result does not depend on how the query's loop was
    run, in parallel or not, even for floating-point `add`. A value sent to
    an id that is not alive, or to an entity without the component, is
    dropped.

    The system must declare the component in `writes`. An apply must be
    inside a query and not inside a loop there, so it runs at most once per
    entity; all applies of one query to the same field use the same rule
    (`add` then `min` would depend on the order). The field must be an
    integer (not `i1`) or a float.
  }];
  let arguments = (ins Ecs_EntityType:$entity, FlatSymbolRefAttr:$component,
                       StrAttr:$field, StrAttr:$rule, AnyType:$value);
  let assemblyFormat = [{
    $entity $component $field custom<Rule>($rule) $value attr-dict
    `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ecs_DespawnOp : Ecs_Op<"despawn", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Remove the visited entity from the world";
  let description = [{
    ```mlir
    ecs.despawn
    ```
    Removes the entity the enclosing query visits. The removal is deferred
    to the end of the query: the query still visits every entity it would
    have visited, and later systems no longer see the removed ones. The
    system must declare every archetype the query matches in `writes`.
  }];
  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

def Ecs_StageOp : Ecs_Op<"stage", [
    HasParent<"ScheduleOp">, NoRegionArguments,
    SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "Runs that do not conflict and may execute in parallel";
  let description = [{
    ```mlir
    ecs.stage {
      ecs.run @gravity(%dt, %g) : f32, f32
      ecs.run @wind(%dt, %w) : f32, f32
    }
    ```
    No two runs in a stage access a common column where one of them
    writes, so any order or interleaving gives the same result. Stages
    execute in program order. `--ecs-schedule` forms stages; a stage
    holds nothing but `ecs.run`.
  }];
  let regions = (region SizedRegion<1>:$body);
  let assemblyFormat = "attr-dict-with-keyword $body";
  let hasVerifier = 1;
}

def Ecs_RunOp : Ecs_Op<"run", [
    ParentOneOf<["ScheduleOp", "StageOp"]>,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run a system as one step of a schedule";
  let arguments = (ins FlatSymbolRefAttr:$system, Variadic<AnyType>:$args);
  let assemblyFormat = "$system `(` $args `)` attr-dict (`:` type($args)^)?";
}

def Ecs_YieldOp : Ecs_Op<"yield", [
    Pure, Terminator,
    ParentOneOf<["SystemOp", "ScheduleOp", "QueryOp", "StageOp"]>]> {
  let summary = "Region terminator; implicit in the custom syntax";
  let assemblyFormat = "attr-dict";
}

#endif // ECS_OPS
