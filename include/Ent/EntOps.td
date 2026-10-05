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
class Ent_RecordOp<string mnemonic, list<Trait> traits = []>
    : Ent_Op<mnemonic, !listconcat([Symbol], traits)> {
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

def Ent_RelationOp : Ent_RecordOp<"relation", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Declare a relation: directed edges between entities, with data";
  let description = [{
    ```mlir
    ent.relation @Synapse (weight: f32) capacity 1000000
    ```
    An edge goes from a source entity to a target entity and carries the
    relation's fields. Edges are not entities: they live in a table of
    their own, holding at most `capacity` edges, kept sorted by source
    (compressed sparse rows), with an index by target where the program
    iterates incoming edges. An entity may have any number of edges, also
    several to the same target. Queries iterate an entity's edges with
    `ent.edges`; `ent.connect` and `ent.disconnect` change them.

    A relation may name the component its sources and its targets have:
    ```mlir
    ent.relation @Synapse (weight: f32) from @Neuron to @Neuron capacity 1000
    ```
    Connecting checks it, at run time, for both ends. Where no system can
    take that component away from an entity (despawn it, or remove the
    component), every edge's end then has it for as long as the edge
    exists, so reading that component of the other end inside an edge loop
    (`ent.lookup`, `ent.apply`) needs no check that the entity is alive and
    has it.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types,
                       OptionalAttr<FlatSymbolRefAttr>:$from,
                       OptionalAttr<FlatSymbolRefAttr>:$to,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity);
  let extraClassDeclaration = [{
    /// The type of field `name`, or null if there is no such field.
    ::mlir::Type getFieldType(::llvm::StringRef name);
    /// The component the edges' sources (`target` false) or targets have,
    /// if the relation names it.
    ::mlir::FlatSymbolRefAttr getEndpoint(bool target) {
      return target ? getToAttr() : getFromAttr();
    }
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

def Ent_ExternOp : Ent_Op<"extern", [
    Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A system implemented outside the program";
  let description = [{
    ```mlir
    ent.extern @report(f32) reads [@Position] writes [@Console]
    ```
    A system without a body: a schedule runs it like any other
    (`ent.run @report(%dt) : f32`), and the lowered program calls the C
    function `ent_report(world, dt)`, which works on the world through the
    generated header (`ent-translate --ent-to-c-header` declares it there).

    The compiler cannot see what it does, so its declared access is its
    access: every field of the components, resources and relations it
    lists, in every archetype holding them, and for an archetype in
    `writes` the set of its entities (it spawns them). Without `reads` or
    `writes` it conflicts with every other system. Effects outside the
    world (output, say) are not in the contract; systems that must keep
    their order because of them write a common resource, or declare
    nothing.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeArrayAttr:$params,
                       OptionalAttr<SymbolRefArrayAttr>:$reads,
                       OptionalAttr<SymbolRefArrayAttr>:$writes);
  let assemblyFormat = [{
    $sym_name `(` custom<ParamTypes>($params) `)` (`reads` $reads^)?
    (`writes` $writes^)? attr-dict
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// Whether it declares its access; without, it is opaque.
    bool hasContract() { return getReadsAttr() || getWritesAttr(); }
    /// The name of the C function a run calls: `ent_` and the system's
    /// name, where a character C cannot have in a name (the `.` after a
    /// module's name) is `_`.
    std::string getCName() {
      std::string name = "ent_";
      for (char c : getSymName())
        name += ::llvm::isAlnum(c) ? c : '_';
      return name;
    }
  }];
}

def Ent_FunctionOp : Ent_Op<"function", [Symbol]> {
  let summary = "A function implemented outside the program";
  let description = [{
    ```mlir
    ent.function @hash(i64) -> i64
    ent.function proc @put(!ent.text<126>)
    ent.function proc @seconds() -> f64
    ```
    A C function a system calls with `ent.invoke`. It takes values and
    gives at most one back; it never sees the world, so what a system
    reads and writes stays what its own body does.

    A plain function gives the same result for the same arguments and
    does nothing else: calls may be dropped, repeated, reordered or run on
    several threads. A `proc` acts on the outside (output, the time); the
    program calls it exactly where and as often as its systems say.

    A bool crosses as a C `bool`, a text as a pointer to its struct
    (`const ent_text126 *`); a text cannot be the result.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeArrayAttr:$params,
                       OptionalAttr<TypeAttr>:$result,
                       UnitAttr:$proc);
  let assemblyFormat = [{
    (`proc` $proc^)? $sym_name `(` custom<ParamTypes>($params) `)`
    (`->` $result^)? attr-dict
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The name of the C function: `ent_` and the function's name, where
    /// a character C cannot have in a name (the `.` after a module's
    /// name) is `_`.
    std::string getCName() {
      std::string name = "ent_";
      for (char c : getSymName())
        name += ::llvm::isAlnum(c) ? c : '_';
      return name;
    }
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
    write a resource, so every entity reads the same value; also in
    conditions and in `ent.main`. A system that
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

def Ent_EdgesOp : Ent_Op<"edges", [
    SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run the body once for every edge of the visited entity";
  let description = [{
    ```mlir
    ent.edges @Synapse out (%s: !ent.ref<@Synapse>, %post: !ent.entity) {
      %w = ent.get %s "weight" : !ent.ref<@Synapse> -> f32
      ent.apply %post @Neuron "input" add %w : f32
    }
    ```
    `out` visits the edges whose source is the entity the enclosing query
    visits, `in` those whose target it is; the second argument is the
    entity at the other end. The first is a ref to the edge's fields, used
    only by `ent.get` and `ent.set`; with `mut` the body may write them:
    every edge is visited once per query, by its source (`out`) or its
    target (`in`), so the write is the visiting entity's own, like a
    component's. Edges are visited in a fixed order (by source, then in the
    order they were connected).

    An `ent.apply` in the body may run once per edge: its values are
    combined when the query ends, by apply, archetype, row and edge, so the
    result does not depend on how the query ran. A query may not iterate a
    relation both ways if either loop writes the edges, and loops do not
    nest. A system that declares its access must list the relation in
    `reads` or `writes` (`writes` for `mut`).
  }];
  let arguments = (ins FlatSymbolRefAttr:$relation, StrAttr:$direction);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    bool isOut() { return getDirection() == "out"; }
  }];
}

def Ent_ConnectOp : Ent_Op<"connect", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Add an edge from one entity to another";
  let description = [{
    ```mlir
    ent.connect @Synapse %pre, %post (%weight) : f32
    ```
    Adds an edge from `%pre` to `%post` with its fields initialised in
    declaration order. Outside a query it takes effect at once. Inside one
    it is deferred to the end of the query, like `ent.apply`, and may run at
    most once per entity (not inside a loop or `ent.edges`); the edges are
    added in a fixed order (by connect, archetype, row). Connecting beyond
    the relation's capacity is an error at run time. Edges whose source or
    target has been despawned are dropped when the edges are next sorted.
    A system that declares its access must list the relation in `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$relation, Ent_EntityType:$source,
                       Ent_EntityType:$target, Variadic<AnyType>:$values);
  let assemblyFormat = [{
    $relation $source `,` $target ` ` `(` $values `)` attr-dict
    (`:` type($values)^)?
  }];
  let hasVerifier = 1;
}

def Ent_DisconnectOp : Ent_Op<"disconnect"> {
  let summary = "Remove the edge the enclosing ent.edges visits";
  let description = [{
    ```mlir
    ent.disconnect
    ```
    The removal is deferred to the end of the query: the query still
    visits the edge in every loop. A system that declares its access must
    list the relation in `writes`.
  }];
  let assemblyFormat = "attr-dict";
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
    once per entity, or once per edge inside `ent.edges` (combined by apply,
    archetype, row, then edge); all applies of one query to the same field use
    the same rule (`add` then `min` would depend on the order). The field must
    be an integer (not `i1`) or a float.
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

def Ent_InvokeOp : Ent_Op<"invoke", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>,
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>]> {
  let summary = "Call a function implemented outside the program";
  let description = [{
    ```mlir
    %h = ent.invoke @hash(%id) : (i64) -> i64
    ent.invoke proc @put(%line) : (!ent.text<126>) -> ()
    ```
    Calls an `ent.function` from inside a system. `proc` repeats what the
    function declares: the call then has effects outside the world, so its
    system keeps its place among all others and a query holding it visits
    its entities in order, one at a time.
  }];
  let arguments = (ins FlatSymbolRefAttr:$callee, Variadic<AnyType>:$args,
                       UnitAttr:$proc);
  let results = (outs Optional<AnyType>:$result);
  let assemblyFormat = [{
    (`proc` $proc^)? $callee `(` $args `)` attr-dict
    `:` functional-type($args, results)
  }];
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

def Ent_MainOp : Ent_Op<"main", [
    IsolatedFromAbove, NoRegionArguments,
    SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "The program's entry point: which schedules run, and when";
  let description = [{
    ```mlir
    ent.main {
      ent.call @setup()
      ent.loop {
        ent.call @frame(%dt) : f32
        %frame = ent.read @Clock "frame" : i32
        %done = arith.cmpi eq, %frame, %last : i32
        ent.yield %done : i1
      }
      ent.call @report()
    }
    ```
    A module with `ent.main` is a program of its own: lowering emits a C
    `main` that creates the world, runs the body and returns 0. The body
    calls schedules (`ent.call`), repeats (`ent.loop`), reads resources and
    computes with ops free of side effects. A module has at most one.
  }];
  let regions = (region SizedRegion<1>:$body);
  let assemblyFormat = "attr-dict-with-keyword $body";
  let hasVerifier = 1;
}

def Ent_CallOp : Ent_Op<"call", [
    ParentOneOf<["MainOp", "LoopOp"]>,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run a schedule once, from the entry point";
  let description = [{
    ```mlir
    ent.call @frame(%dt) : f32
    ```
  }];
  let arguments = (ins FlatSymbolRefAttr:$schedule, Variadic<AnyType>:$args);
  let assemblyFormat = [{
    $schedule `(` $args `)` attr-dict (`:` type($args)^)?
  }];
}

def Ent_LoopOp : Ent_Op<"loop", [
    ParentOneOf<["MainOp", "LoopOp"]>, NoRegionArguments,
    SingleBlockImplicitTerminator<"YieldOp">]> {
  let summary = "Repeat a part of the entry point until a condition holds";
  let description = [{
    ```mlir
    ent.loop {
      ent.call @frame()
      %done = ent.read @Exit "value" : i1
      ent.yield %done : i1
    }
    ```
    Runs the body, then stops if it yielded true; so the body runs at
    least once, and the condition sees what it did. A body that yields
    nothing repeats forever.
  }];
  let regions = (region SizedRegion<1>:$body);
  let assemblyFormat = "attr-dict-with-keyword $body";
  let hasVerifier = 1;
}

def Ent_YieldOp : Ent_Op<"yield", [
    Pure, Terminator,
    ParentOneOf<["SystemOp", "ScheduleOp", "QueryOp", "StageOp", "RunOp",
                 "EdgesOp", "MainOp", "LoopOp"]>]> {
  let summary = "Region terminator; implicit in the custom syntax";
  let description = [{
    Ends a body without operands, and a condition with its value.
  }];
  let arguments = (ins Variadic<AnyType>:$results);
  let builders = [OpBuilder<(ins), [{ /* nothing to do */ }]>];
  let assemblyFormat = "attr-dict ($results^ `:` type($results))?";
}

#endif // ENT_OPS
