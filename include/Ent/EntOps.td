#ifndef ENT_OPS
#define ENT_OPS

include "Ent/EntDialect.td"
include "Ent/EntTypes.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

// A symbol that declares an ordered list of scalar fields.
class Ent_RecordOp<string mnemonic> : Ent_Op<mnemonic, [Symbol]> {
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

def Ent_ComponentOp : Ent_RecordOp<"component"> {
  let summary = "Declare a component: an ordered list of scalar fields";
  let description = [{
    ```mlir
    ent.component @Position (x: f32, y: f32) capacity 100000
    ```
    `capacity` bounds how many entities can have the component at once. An
    archetype the compiler infers from spawns holds as many entities as the
    smallest capacity among its required components allows.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types,
                       OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:
                           $capacity);
}

def Ent_ResourceOp : Ent_RecordOp<"resource"> {
  let summary = "Declare a resource: world state that exists exactly once";
  let description = [{
    ```mlir
    ent.resource @Clock (dt: f32, frame: i64)
    ```
    A resource is not an entity: no query visits it. Systems name it in
    their `reads` and `writes` like a component and access its fields with
    `ent.read` and `ent.write`. It lives in the world's storage next to the
    entity counts.
  }];
}

def Ent_ArchetypeOp : Ent_Op<"archetype", [
    Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Declare a set of components that entities are stored by";
  let description = [{
    ```mlir
    ent.archetype @Body (@Position, @Velocity, @Mass) capacity 100000
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
    ent.archetype @Character (@Position, @Velocity, optional @Stunned)
        capacity 100000
    ```
    It is stored as its field columns plus a presence byte per entity, so
    gaining or losing it (`ent.add`, `ent.remove`) is a write to the
    entity's own row rather than a move to another archetype. Queries that
    bind it run only for the entities that currently have it.

    Archetypes need not be declared: `--ent-infer-archetypes` derives them
    from spawns (`ent.spawn (@A, @B)(...)`) and adds them, marked
    `inferred`. A declared archetype with exactly a spawn's components is
    used instead, which names it and sets its capacity.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       FlatSymbolRefArrayAttr:$components,
                       OptionalAttr<FlatSymbolRefArrayAttr>:$optional,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity,
                       UnitAttr:$inferred);
  let assemblyFormat = [{
    $sym_name custom<ComponentList>($components, $optional)
    `capacity` $capacity (`inferred` $inferred^)? attr-dict
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// True if the archetype stores `component`, always or optionally.
    bool contains(::mlir::FlatSymbolRefAttr component);
    /// True if `component` is optional in this archetype.
    bool isOptional(::mlir::FlatSymbolRefAttr component);
  }];
}

def Ent_SystemOp : Ent_Op<"system", [
    Symbol, IsolatedFromAbove, SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A system: queries and code that run as one step";
  let description = [{
    ```mlir
    ent.system @integrate(%dt: f32) {
      ent.query (%p: !ent.ref<@Position, mut>, %v: !ent.ref<@Velocity>) {
        ...
      }
    }
    ```
    What a system reads and writes is inferred from its body. It may also
    declare it, as a contract the compiler checks:
    ```mlir
    ent.system @integrate(%dt: f32) reads [@Velocity] writes [@Position] {
    ```
    A system that declares either list (even empty, `reads []`) must stay
    within both. `writes` implies read access, and may also name archetypes:
    the system changes their set of entities (`ent.spawn`, `ent.despawn`).
    Entry block arguments are the system's per-run parameters.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       OptionalAttr<SymbolRefArrayAttr>:$reads,
                       OptionalAttr<SymbolRefArrayAttr>:$writes);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// Whether the system declares its access (`reads` or `writes`, even
    /// empty): then its body must stay within the declaration. Without one,
    /// its access is whatever the body does.
    bool hasContract();
    bool canRead(::mlir::FlatSymbolRefAttr component);
    bool canWrite(::mlir::FlatSymbolRefAttr component);
  }];
}

def Ent_ScheduleOp : Ent_Op<"schedule", [
    Symbol, IsolatedFromAbove, SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "An ordered list of systems that make up one frame";
  let description = [{
    ```mlir
    ent.schedule @frame(%dt: f32) {
      ent.run @integrate(%dt) : f32
    }
    ```
    Program order is the semantic order; a scheduler may reorder or
    parallelise runs whose access sets do not conflict.

    A schedule may run only under a condition, evaluated when it starts:
    ```mlir
    ent.schedule @frame(%dt: f32) if {
    ^bb0(%dt: f32):
      %paused = ent.read @Paused "value" : i1
      %run = arith.xori %paused, %true : i1
      ent.yield %run : i1
    } {
      ...
    }
    ```
    The condition's block takes the schedule's parameters; it may read
    resources and compute with ops free of side effects.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region SizedRegion<1>:$body, AnyRegion:$condition);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Operations inside systems and schedules
//===----------------------------------------------------------------------===//

def Ent_QueryOp : Ent_Op<"query", [
    HasParent<"SystemOp">, SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run the body once for every entity that has all components";
  let description = [{
    Each entry block argument is an `!ent.ref` to a distinct component, and
    refs are only used by `ent.get` and `ent.set`. Iteration order is
    unspecified, so the body must not depend on it.

    A reactive query lists triggers after its arguments:
    ```mlir
    ent.query (%h: !ent.ref<@Hull>, %b: !ent.ref<@Bar, mut>)
        on [changed @Hull "hp", added @Hull] { ... }
    ```
    It runs only for the entities that had at least one of these events
    since the query last started, and that match the query now (an entity
    may have changed again, or moved, since): `added @C` when the entity
    gained `C` (by `ent.add` or by being spawned with it), `removed @C`
    when it lost `C` while staying alive, `changed @C "f"` on every write
    to field `f` (`ent.set`, `ent.apply`, `ent.add`, spawning), and
    `changed @C` on a write to any field of `C`. On its first run every
    existing entity counts as added and changed. Events the query causes
    itself count on its next run. A system that declares its access must
    list every trigger's component in `reads` or `writes`.

    Filters narrow the entities a query visits without binding components:
    ```mlir
    ent.query (%p: !ent.ref<@Position, mut>) with [@Enemy]
        without [@Shield] any [@Fire, @Ice] { ... }
    ```
    The entity must have every `with` component, none of the `without`
    components, and at least one component of each `any` group. A filter
    reads only whether the entity has the component, never its fields, so
    it does not conflict with systems that write them. A query binds or
    filters by at least one component.
  }];
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The attribute holding a reactive query's triggers: an array of
    /// [kind, component, field] entries, the field empty unless the kind
    /// is "changed" with a field.
    static constexpr ::llvm::StringLiteral kTriggersAttr = "triggers";
    /// Filters: arrays of components (`any`: an array of such arrays).
    static constexpr ::llvm::StringLiteral kWithAttr = "with";
    static constexpr ::llvm::StringLiteral kWithoutAttr = "without";
    static constexpr ::llvm::StringLiteral kAnyAttr = "any";

    /// The components the entity must have: those bound, then `with`.
    ::llvm::SmallVector<::mlir::FlatSymbolRefAttr> getRequired();
    /// The components the entity must not have.
    ::llvm::SmallVector<::mlir::FlatSymbolRefAttr> getWithout();
    /// Groups of components of which the entity must have one each.
    ::llvm::SmallVector<::llvm::SmallVector<::mlir::FlatSymbolRefAttr>>
    getAnyGroups();
  }];
}

def Ent_GetOp : Ent_Op<"get", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read one field through a component reference";
  let arguments = (ins Ent_RefType:$ref, StrAttr:$field);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$ref $field attr-dict `:` type($ref) `->` type($result)";
}

def Ent_SetOp : Ent_Op<"set", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Write one field through a mutable component reference";
  let arguments = (ins Ent_RefType:$ref, StrAttr:$field, AnyType:$value);
  let assemblyFormat =
      "$ref $field `,` $value attr-dict `:` type($ref) `,` type($value)";
  let hasVerifier = 1;
}

def Ent_ReadOp : Ent_Op<"read", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read one field of a resource";
  let description = [{
    ```mlir
    %dt = ent.read @Clock "dt" : f32
    ```
    Allowed anywhere in a system, including inside queries: no query may
    write a resource, so every entity reads the same value. A system that
    declares its access must list the resource in `reads` or `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$resource $field attr-dict `:` type($result)";
  let hasVerifier = 1;
}

def Ent_WriteOp : Ent_Op<"write", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Write one field of a resource, outside queries";
  let description = [{
    ```mlir
    ent.write @Clock "frame", %next : i64
    ```
    Allowed in a system but not inside a query: a write per entity would
    make every entity depend on the others, which rules out parallel and
    fused loops. A system that declares its access must list the resource in
    `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field,
                       AnyType:$value);
  let assemblyFormat =
      "$resource $field `,` $value attr-dict `:` type($value)";
  let hasVerifier = 1;
}

def Ent_AddOp : Ent_Op<"add", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Give the visited entity a component";
  let description = [{
    ```mlir
    ent.add @Stunned(%seconds) : f32
    ```
    Gives the entity the enclosing query visits the component, with its
    fields initialised in declaration order. What that means depends on
    how the entity's archetype stores the component, not on the program:
    - optional in the archetype: set its presence (and fields);
    - always held by the archetype: overwrite the fields;
    - not held: move the entity to the archetype with exactly its
      components plus this one, which must exist. Moves are deferred to
      the end of the query, like despawns.
    A system that declares its access must list the component in `writes`; a
    move needs no archetypes declared, since whether a change moves entities is
    a storage decision and the analysis derives its effect from the storage.
  }];
  let arguments = (ins FlatSymbolRefAttr:$component,
                       Variadic<AnyType>:$values);
  let assemblyFormat =
      "$component `(` $values `)` attr-dict (`:` type($values)^)?";
  let hasVerifier = 1;
}

def Ent_RemoveOp : Ent_Op<"remove", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Take a component away from the visited entity";
  let description = [{
    ```mlir
    ent.remove @Stunned
    ```
    Takes the component away from the entity the enclosing query visits:
    clears its presence where it is optional, moves the entity to the
    archetype with exactly its components minus this one where it is not
    (deferred to the end of the query), and does nothing where the
    archetype does not hold it. The same rules as for `ent.add` apply.
  }];
  let arguments = (ins FlatSymbolRefAttr:$component);
  let assemblyFormat = "$component attr-dict";
  let hasVerifier = 1;
}

def Ent_SpawnOp : Ent_Op<"spawn", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Create an entity and return its id";
  let description = [{
    ```mlir
    ent.spawn (@Position, @Velocity)(%x, %dx) : f32, f32
    ent.spawn @Bullet(%x, %dx, %seconds) : f32, f32, f32
    ```
    The first form lists the components the new entity has; the values
    initialise their fields, in that order. The archetype it lives in is
    inferred (`--ent-infer-archetypes`, which records it: `ent.spawn
    (@Position, @Velocity) into @Position_Velocity (...)`). The second form
    names a declared archetype; the values initialise every field of its
    non-optional components, in the order of the components and then of
    their fields, and optional components start absent.

    Appends the entity to the archetype. Spawning beyond the archetype's
    capacity is an error at run time. A system that declares its access
    must list the spawned components in `writes`, or the archetype for the
    second form.

    The result is the entity's id, which stays valid while the entity moves
    between rows and archetypes and becomes invalid when it is despawned.
  }];
  let arguments = (ins OptionalAttr<FlatSymbolRefAttr>:$archetype,
                       OptionalAttr<FlatSymbolRefArrayAttr>:$components,
                       Variadic<AnyType>:$values);
  let results = (outs Ent_EntityType:$entity);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// Whether the new entity has `component` from the start: listed (first
    /// form), or held by the archetype non-optionally (second form).
    bool startsWith(::mlir::FlatSymbolRefAttr component);
  }];
}

def Ent_EntityOp : Ent_Op<"entity"> {
  let summary = "The id of the entity the enclosing query visits";
  let description = [{
    ```mlir
    %id = ent.entity
    ```
    The same id `ent.spawn` returned for the entity.
  }];
  let results = (outs Ent_EntityType:$entity);
  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

def Ent_HasOp : Ent_Op<"has", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Whether the visited entity has a component";
  let description = [{
    ```mlir
    %shielded = ent.has @Shield
    ```
    True if the entity the enclosing query visits had the component when
    the query started: the query's own `ent.add` and `ent.remove` take
    effect for it at its end, whether they move the entity or not. So the
    answer does not depend on how archetypes store the component. It is a
    constant where the archetype always or never holds the component, and
    reads the presence where it is optional. A system that declares its
    access must list the component in `reads` or `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$component);
  let results = (outs I1:$result);
  let assemblyFormat = "$component attr-dict";
  let hasVerifier = 1;
}

def Ent_LookupOp : Ent_Op<"lookup", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Read a field of another entity, by id";
  let description = [{
    ```mlir
    %x, %found = ent.lookup %target @Position "x" : f32
    ```
    Reads field `x` of `@Position` of the entity with id `%target`, in
    whatever archetype it lives. `%found` is false, and the value 0, if
    the id is not alive or the entity does not have the component (its
    archetype does not hold it, or holds it optionally and it is absent).
    A component field of type `!ent.entity` is how entities refer to each
    other.

    A system that declares its access must list the component in `reads` or
    `writes`. A query may not look up a field it writes itself: which entities
    would see the old value and which the new would depend on iteration order.
    Lookups only read; a system with lookups is not fused with others, for the
    same reason.
  }];
  let arguments = (ins Ent_EntityType:$entity, FlatSymbolRefAttr:$component,
                       StrAttr:$field);
  let results = (outs AnyType:$value, I1:$found);
  let assemblyFormat = [{
    $entity $component $field attr-dict `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ent_ApplyOp : Ent_Op<"apply", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Combine a value into a field of another entity, by id";
  let description = [{
    ```mlir
    ent.apply %target @Hull "hp" add %damage : f32
    ```
    Combines `%damage` into field `hp` of `@Hull` of the entity with id
    `%target`, with the rule `add`, `min` or `max` (signed for integers).
    Applies are deferred to the end of the enclosing query: then every
    target's field becomes `field ⊕ v1 ⊕ v2 ⊕ ...` over the values sent to
    it, before the query's despawns and moves take effect. So the query
    itself sees every target's old value and may read the field (with
    `ent.get` or `ent.lookup`) or set it; applies land on top.

    The values are combined in a fixed order (by apply, then archetype,
    then row), so the result does not depend on how the query's loop was
    run, in parallel or not, even for floating-point `add`. A value sent to
    an id that is not alive, or to an entity without the component, is
    dropped.

    A system that declares its access must list the component in `writes`. An
    apply must be inside a query and not inside a loop there, so it runs at most
    once per entity; all applies of one query to the same field use the same
    rule (`add` then `min` would depend on the order). The field must be an
    integer (not `i1`) or a float.
  }];
  let arguments = (ins Ent_EntityType:$entity, FlatSymbolRefAttr:$component,
                       StrAttr:$field, StrAttr:$rule, AnyType:$value);
  let assemblyFormat = [{
    $entity $component $field custom<Rule>($rule) $value attr-dict
    `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ent_AccumulateOp : Ent_Op<"accumulate", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Combine a value into a resource field, from inside a query";
  let description = [{
    ```mlir
    ent.accumulate @Score "points" add %bonus : i64
    ```
    Combines `%bonus` into field `points` of `@Score` with the rule `add`,
    `min` or `max` (signed for integers). A resource cannot be written
    inside a query, where every entity would write the same field; an
    accumulate is the commutative alternative. Like `ent.apply`, its values
    are combined when the query ends, in a fixed order (by accumulate, then
    archetype, then row), so the result does not depend on how the query
    ran, in parallel or not, even for floating-point `add`; reads of the
    resource in the same query see the value it had when the query started.

    A system that declares its access must list the resource in `writes`. An
    accumulate must be inside a query and not inside a loop there, all
    accumulates of one query to the same field use the same rule, and the field
    must be an integer (not `i1`) or a float.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field,
                       StrAttr:$rule, AnyType:$value);
  let assemblyFormat = [{
    $resource $field custom<Rule>($rule) $value attr-dict `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ent_DespawnOp : Ent_Op<"despawn", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Remove the visited entity from the world";
  let description = [{
    ```mlir
    ent.despawn
    ```
    Removes the entity the enclosing query visits. The removal is deferred
    to the end of the query: the query still visits every entity it would
    have visited, and later systems no longer see the removed ones. A
    system that declares its access must list every archetype the query
    matches in `writes`.
  }];
  let assemblyFormat = "attr-dict";
  let hasVerifier = 1;
}

def Ent_StageOp : Ent_Op<"stage", [
    HasParent<"ScheduleOp">, NoRegionArguments,
    SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "Runs that do not conflict and may execute in parallel";
  let description = [{
    ```mlir
    ent.stage {
      ent.run @gravity(%dt, %g) : f32, f32
      ent.run @wind(%dt, %w) : f32, f32
    }
    ```
    No two runs in a stage access a common column where one of them
    writes, so any order or interleaving gives the same result. Stages
    execute in program order. `--ent-schedule` forms stages; a stage
    holds nothing but `ent.run`.
  }];
  let regions = (region SizedRegion<1>:$body);
  let assemblyFormat = "attr-dict-with-keyword $body";
  let hasVerifier = 1;
}

def Ent_RunOp : Ent_Op<"run", [
    ParentOneOf<["ScheduleOp", "StageOp"]>,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run a system as one step of a schedule";
  let description = [{
    ```mlir
    ent.run @move(%dt) : f32
    ent.run @move(%dt) : f32 if {
      %paused = ent.read @Paused "value" : i1
      %run = arith.xori %paused, %true : i1
      ent.yield %run : i1
    }
    ```
    With a condition, the system runs only if it holds when the run would
    start (after the runs before it). The condition may read resources and
    compute with ops free of side effects; its reads count as the run's.
  }];
  let arguments = (ins FlatSymbolRefAttr:$system, Variadic<AnyType>:$args);
  let regions = (region AnyRegion:$condition);
  let assemblyFormat = [{
    $system `(` $args `)` attr-dict (`:` type($args)^)?
    (`if` $condition^)?
  }];
  let hasVerifier = 1;
}

def Ent_YieldOp : Ent_Op<"yield", [
    Pure, Terminator,
    ParentOneOf<["SystemOp", "ScheduleOp", "QueryOp", "StageOp", "RunOp"]>]> {
  let summary = "Region terminator; implicit in the custom syntax";
  let description = [{
    Ends a body without operands, and a condition with its value.
  }];
  let arguments = (ins Variadic<AnyType>:$results);
  let builders = [OpBuilder<(ins), [{ /* nothing to do */ }]>];
  let assemblyFormat = "attr-dict ($results^ `:` type($results))?";
}

#endif // ENT_OPS
