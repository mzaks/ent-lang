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

    With `apart` (`ent.component @Close () apart capacity 1000`) the
    entities that have the component are stored apart from those that do
    not: no inferred archetype holds it optionally, and `ent.add` and
    `ent.remove` of it move the entity to an archetype the compiler infers
    for the purpose (see `inferArchetypes`).
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types,
                       OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:
                           $capacity,
                       UnitAttr:$apart);
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

def Ent_BufferOp : Ent_RecordOp<"buffer"> {
  let summary = "Declare a buffer: rows the program fills, in their order";
  let description = [{
    ```mlir
    ent.buffer @Shapes (kind: i32, x: f32, y: f32) capacity 4096
    ```
    A list of rows, each with a value for every field, that the program
    appends to (`ent.append`), empties (`ent.clear`) and reads
    (`ent.buffer.len`, `ent.buffer.at`), and that a function in C is
    handed whole (`ent.buffer.of`). It is no entity's: no query visits it.
    It is stored field by field, the values of one field one after
    another, and holds at most `capacity` rows. A field is a number, a
    bool or an enum. Systems name it in `reads` and `writes` like a
    resource.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity);
}

def Ent_AppendOp : Ent_Op<"append", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Add a row to a buffer";
  let description = [{
    ```mlir
    ent.append @Shapes (%kind, %x, %y) : i32, f32, f32
    ```
    Adds a row after those the buffer has, with its fields in declaration
    order. Outside a query it is there at once. Inside one it is deferred
    to the end of the query, like `ent.connect`, and may run at most once
    per entity (not inside a loop or `ent.edges`); the rows are added in a
    fixed order (by append, archetype, row), however the query ran.
    Appending beyond the buffer's capacity is an error at run time. A
    system that declares its access must list the buffer in `writes`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, Variadic<AnyType>:$values);
  let assemblyFormat = [{
    $buffer `(` $values `)` attr-dict (`:` type($values)^)?
  }];
  let hasVerifier = 1;
}

def Ent_ClearOp : Ent_Op<"clear", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Empty a buffer";
  let description = [{
    ```mlir
    ent.clear @Shapes
    ```
    The buffer has no rows after it. In a system, outside its queries.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer);
  let assemblyFormat = "$buffer attr-dict";
}

def Ent_ClusterOp
    : Ent_Op<"cluster", [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary =
      "Move an archetype's rows into the order of its clustered index";
  let description = [{
    ```mlir
    ent.cluster @Order
    ```
    `@Order` is a sorted index declared `clustered` (its `ent.index` has
    `clustered`), just made: the rows of the one archetype that holds its
    entities move so that the entity of the index's row r is at row r,
    every column and the ids, the entity table following; each row's place
    (`place_in_Name`, and the index row's `place`) is its row then. In a
    system, outside its queries: the index's own update.
  }];
  let arguments = (ins FlatSymbolRefAttr:$index);
  let assemblyFormat = "$index attr-dict";
}

def Ent_BufferLenOp : Ent_Op<"buffer.len", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "How many rows a buffer has";
  let description = [{
    ```mlir
    %n = ent.buffer.len @Shapes
    ```
    In a query, how many it had when the query started.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer);
  let results = (outs I32:$result);
  let assemblyFormat = "$buffer attr-dict";
}

def Ent_BufferAtOp : Ent_Op<"buffer.at", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A value of a row of a buffer";
  let description = [{
    ```mlir
    %x = ent.buffer.at @Shapes "x"[%i : i32] : f32
    ```
    What row `%i` has in that field; nought (the first case of an enum)
    where the buffer has no such row. By an `!ent.index_of` of the
    buffer, the row as it is in the buffer's room, which it is masked
    to, whether the buffer has it or not.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, StrAttr:$field,
                       AnyTypeOf<[AnySignlessInteger, Ent_IndexOfType]>:$index);
  let results = (outs AnyType:$result);
  let assemblyFormat = [{
    $buffer $field `[` $index `:` type($index) `]` attr-dict `:` type($result)
  }];
}

def Ent_IndexGetOp
    : Ent_Op<"index.get", [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "The entity of a key, of a hash index";
  let description = [{
    ```mlir
    %e = ent.index.get @ById [%k : i64] : !ent.entity
    %f = ent.index.get @ByName [%h, %c : i64] : !ent.entity
    ```
    Of the hash index whose slots are the buffer `@ById` (its rows
    `@ById_list`, its tags `@ById_tags`): the entity with the key, the
    first as they are stored where several have it, none where none has
    it. A text key is given by its two hashes (`hash_of(t)` and
    `hash_of(t, 0x9e3779b97f4a7c15)`). Reads the index's buffers; not in
    a `gpu` system.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, AnyType:$key,
      Optional<I64>:$check);
  let results = (outs Ent_EntityType:$result);
  let assemblyFormat = [{
    $buffer `[` $key (`,` $check^)? `:` type($key) `]` attr-dict `:`
    type($result)
  }];
  let hasVerifier = 1;
}

def Ent_BufferSetOp : Ent_Op<"buffer.set", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Give a row of a buffer another value";
  let description = [{
    ```mlir
    ent.buffer.set @Shapes "x"[%i : i32] = %x : f32
    ```
    Row `%i` has `%x` in that field after it; nothing happens where the
    buffer has no such row (by an `!ent.index_of` of the buffer, the row
    in its room is set, as `ent.buffer.at` reads it). Outside queries, in
    a system that may write the buffer or in a proc that was given it
    `mut`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, StrAttr:$field,
                       AnyTypeOf<[AnySignlessInteger, Ent_IndexOfType]>:$index,
                       AnyType:$value);
  let assemblyFormat = [{
    $buffer $field `[` $index `:` type($index) `]` `=` $value attr-dict
    `:` type($value)
  }];
}

def Ent_BufferFillOp : Ent_Op<"buffer.fill", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Give a buffer so many rows, all alike";
  let description = [{
    ```mlir
    ent.buffer.fill @Shapes (%kind, %x, %y) times %n : i32, f32, f32
    ```
    The buffer has `%n` rows after it (none where `%n` is below nought),
    each with these values, in the order of the fields; what it had is
    gone. More rows than it holds is an error at run time. Outside
    queries, like `ent.buffer.set`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, Variadic<AnyType>:$values,
                       I32:$count);
  let assemblyFormat = [{
    $buffer `(` $values `)` `times` $count attr-dict (`:` type($values)^)?
  }];
}

def Ent_BufferKeepOp : Ent_Op<"buffer.keep", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Keep the first rows of a buffer";
  let description = [{
    ```mlir
    ent.buffer.keep @Arena %n
    ```
    The buffer has its first `%n` rows after it, as they were, and none
    after them: all it has where it has no more than `%n`, none where
    `%n` is below nought. Outside queries, like `ent.buffer.set`.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, I32:$count);
  let assemblyFormat = "$buffer $count attr-dict";
}

def Ent_BufferClaimOp : Ent_Op<"buffer.claim", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Take rows of a buffer as the visited entity's own";
  let description = [{
    ```mlir
    ent.buffer.claim @Cohorts %start, %count
    ```
    In a query (not one that takes turns), at the start of its body: rows
    `%start` to `%start + %count` (but not that) are the visited entity's
    for this run of the query, which reads and writes them through
    `ent.buffer.at` and `ent.buffer.set` marked `ent.window` (a window,
    `w: mut Cohorts[start, count]`), and no other access of the buffer.
    Where another entity of the run took one of them too, or the rows are
    not all rows the buffer has, the query's end stops the program.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, I32:$start, I32:$count);
  let assemblyFormat = "$buffer $start `,` $count attr-dict";
}

def Ent_BufferRowsOp : Ent_Op<"buffer.rows", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Rows of a buffer, to hand to a function of the program";
  let description = [{
    ```mlir
    %w = ent.buffer.rows @Cohorts %start, %count : !ent.rows<@Cohorts, mut>
    ```
    Rows `%start` to `%start + %count` of the buffer, for an `ent.invoke`
    of a fn or proc with a body that takes `!ent.rows`: a window of the
    query it is in (taken, where it is `mut`), or all the rows the buffer
    has, outside queries (or, to read, in one).
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer, I32:$start, I32:$count);
  let results = (outs Ent_RowsType:$result);
  let assemblyFormat = "$buffer $start `,` $count attr-dict `:` type($result)";
}

def Ent_RowsStartOp : Ent_Op<"rows.start", [Pure]> {
  let summary = "Where rows a function was given start in their buffer";
  let arguments = (ins Ent_RowsType:$rows);
  let results = (outs I32:$result);
  let assemblyFormat = "$rows attr-dict `:` type($rows)";
}

def Ent_RowsCountOp : Ent_Op<"rows.count", [Pure]> {
  let summary = "How many rows a function was given";
  let arguments = (ins Ent_RowsType:$rows);
  let results = (outs I32:$result);
  let assemblyFormat = "$rows attr-dict `:` type($rows)";
}

def Ent_BufferOfOp : Ent_Op<"buffer.of", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A buffer, to hand to a function in C";
  let description = [{
    ```mlir
    %shapes = ent.buffer.of @Shapes : !ent.buffer<@Shapes>
    ent.invoke proc @draw(%shapes) : (!ent.buffer<@Shapes>) -> ()
    ```
    The buffer, for an `ent.invoke` next to it of a function that takes
    it. One without a body (in C) is handed it outside queries, reads its
    rows while it runs and keeps nothing; one with a body may be given it
    in a query too, to read. As `!ent.buffer<@Shapes, mut>` it is given to
    be changed, outside queries, and the system writes the buffer.
  }];
  let arguments = (ins FlatSymbolRefAttr:$buffer);
  let results = (outs Ent_BufferType:$result);
  let assemblyFormat = "$buffer attr-dict `:` type($result)";
}

def Ent_EnumOp : Ent_Op<"enum", [Symbol]> {
  let summary = "Declare an enum: a type whose values are named cases";
  let description = [{
    ```mlir
    ent.enum @Way ["Right", "Down", "Left", "Up"]
    ```
    A value of the type `!ent.enum<@Way>` is one of the cases, stored as
    the byte that numbers it: `Right` is 0, `Down` 1, and so on. Fields,
    parameters and results may have the type. A generated C header names
    the cases (`ent_Way_Right`). With the attribute `bits` its values are of
    that many bits and more than its cases, which only name the first ones
    (`!ent.enum<@Rows, 32>`).
  }];
  let arguments = (ins SymbolNameAttr:$sym_name, StrArrayAttr:$cases);
  let assemblyFormat = "$sym_name $cases attr-dict";
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The name of the C type: `ent_` and the enum's name, where a
    /// character C cannot have in a name (the `.` after a module's name)
    /// is `_`.
    std::string getCName() {
      std::string name = "ent_";
      for (char c : getSymName())
        name += ::llvm::isAlnum(c) ? c : '_';
      return name;
    }
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

    A relation may be a tree:
    ```mlir
    ent.relation @ChildOf () from @Node to @Node tree capacity 1000
    ```
    Then an entity has at most one outgoing edge, to its parent, and no
    entity is its own ancestor. Connecting an entity that has a parent
    gives it the new one instead (the last connect wins); edges that close
    a cycle stop the program. Queries may bind a component of an ancestor
    along a tree (`!ent.ref<@C, up @ChildOf>`) and visit parents before
    their children (`cascade @ChildOf`).

    A tree may ask for its entities to be stored in its order:
    ```mlir
    ent.relation @Flows () from @Node to @Node tree sorted capacity 100000
    ```
    Then the rows of the archetypes that hold its entities are kept in the
    tree's order (the entities without a parent first, then the others by
    their depth), so a query that cascades along the tree reads the rows
    one after another and finds each entity's parent where it is. The order is restored whenever the tree's edges change or
    the archetype gains or loses an entity, which moves every row: what a
    program combines in the order of rows (`ent.apply`, `ent.accumulate`)
    then depends on the tree, a host cannot keep rows, and ids are never
    rows in such a program. The tree names what its sources and targets
    have (`from`, `to`): the archetypes holding either are the sorted ones.
    No archetype is in two sorted trees: its rows have one order.

    `tree ordered by @Slot "at"` gives the children of an entity an order:
    by the integer field `at` of their component `@Slot` (0 for a child
    without it), smallest first, and by their ids where that is the same.
    A cascading query then visits the children of an entity in that order,
    an edge loop over the edges into an entity too, and a ref `before` the
    tree (`!ent.ref<@C, before @R>`) leads to the child before. The order
    is restored before the next query after a connect or a write to the
    field. Together with `sorted`, the rows are in the tree's order with
    the children of an entity in theirs.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types,
                       OptionalAttr<FlatSymbolRefAttr>:$from,
                       OptionalAttr<FlatSymbolRefAttr>:$to,
                       UnitAttr:$tree,
                       UnitAttr:$sorted,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity,
                       OptionalAttr<FlatSymbolRefAttr>:$order_component,
                       OptionalAttr<StrAttr>:$order_field);
  let extraClassDeclaration = [{
    /// The type of field `name`, or null if there is no such field.
    ::mlir::Type getFieldType(::llvm::StringRef name);
    /// Whether the children of an entity have an order (`ordered by`).
    bool isOrdered() { return static_cast<bool>(getOrderComponentAttr()); }
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

    With `gpu` its queries run on the GPU where the program is built for
    one, and on the CPU otherwise, with the same results:
    ```mlir
    ent.system @integrate(%dt: f32) gpu {
    ```
    That is a contract too: every query of the system only touches the
    entity it visits (its components, its id, filters, in-place `ent.add`
    and `ent.remove`) and reads what every entity reads alike (resources,
    buffers, the program's own functions), and the verifier says which op
    keeps one from running there. The system's code outside its queries
    runs on the CPU.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       OptionalAttr<SymbolRefArrayAttr>:$reads,
                       OptionalAttr<SymbolRefArrayAttr>:$writes,
                       UnitAttr:$gpu);
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

def Ent_FunctionOp : Ent_Op<"function", [Symbol, IsolatedFromAbove]> {
  let summary = "A function over values: the program's own, or one in C";
  let description = [{
    ```mlir
    ent.function @hash(i64) -> i64
    ent.function proc @put(!ent.text<126>)
    ent.function proc @seconds() -> f64
    ent.function @square(%x: i32) -> i32 {
      %0 = arith.muli %x, %x : i32
      ent.yield %0 : i32
    }
    ent.function @swap(%a: i32, %b: f32) -> (f32, i32) {
      ent.yield %b, %a : f32, i32
    }
    ```
    A function a system calls with `ent.invoke`. It takes values and
    gives at most one back; it never sees the world, so what a system
    reads and writes stays what its own body does.

    A plain function gives the same result for the same arguments and
    does nothing else: calls may be dropped, repeated, reordered or run on
    several threads. A `proc` acts on the outside (output, the time); the
    program calls it exactly where and as often as its systems say.

    Without a body it is implemented in C. A bool then crosses as a C
    `bool`, a text as a pointer to its struct (`const ent_text126 *`); a
    text cannot be the result.

    With a body it is the program's own: the entry block's arguments are
    its parameters, and `ent.yield` ends it with its value. The body only
    computes: of this dialect it may hold calls of other plain functions
    (also of itself) and nothing else. A value of any type may go in and
    come out, and it may give several values (`-> (f32, i32)`), which
    `ent.yield` and its calls then have in that order. Only a plain
    function has a body so far.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeArrayAttr:$params,
                       TypeArrayAttr:$results,
                       UnitAttr:$proc);
  let regions = (region AnyRegion:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let hasRegionVerifier = 1;
  let extraClassDeclaration = [{
    /// Whether the function has a body; without one it is implemented in C.
    bool isDefined() { return !getBody().empty(); }

    /// What it gives back, if that is one value.
    std::optional<::mlir::Type> getResult() {
      if (getResults().size() != 1)
        return std::nullopt;
      return ::llvm::cast<::mlir::TypeAttr>(getResults()[0]).getValue();
    }
    ::llvm::SmallVector<::mlir::Type> getResultTypes() {
      return ::llvm::SmallVector<::mlir::Type>(
          getResults().getAsValueRange<::mlir::TypeAttr>());
    }

    /// The name of the lowered function, for one without a body the C
    /// function's: `ent_` and the function's name, where a character C
    /// cannot have in a name (the `.` after a module's name) is `_`.
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
    SingleBlockImplicitTerminator<"YieldOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run the body once for every entity that has all components";
  let description = [{
    Each entry block argument is an `!ent.ref` to a distinct component, and
    refs are only used by `ent.get` and `ent.set`. Iteration order is
    unspecified, so the body must not depend on it.

    A query is an op of a system's body, or of a loop (`scf.for`,
    `scf.while`) there, which runs it so many times: a reactive query then
    reacts each time to what has changed since the time before.

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

    A ref may be to a component of an ancestor along a tree relation:
    ```mlir
    ent.query (%b: !ent.ref<@Body, mut>, %p: !ent.ref<@Body, up @Orbits>)
        cascade @Orbits { ... }
    ```
    `%p` is `@Body` of the nearest ancestor of the visited entity that has
    it: of its parent along `@Orbits`, else of the parent's parent, and so
    on. The query visits only the entities with such an ancestor; the
    entity itself need not have the component. Like a lookup it must not
    read a field the query writes, unless the query cascades along the same
    relation.

    `cascade @Orbits` orders the query by depth in the tree: it runs as if
    it were one query per depth, first for the entities without a parent,
    then for their children, and so on, each seeing what those before it
    wrote. So a ref `up @Orbits` reads what this query already wrote to the
    ancestor. `cascade @Orbits leaves first` runs the depths the other way,
    the deepest first and the entities without a parent last: children
    before their parents. Either way a `mut` ref up the tree lets the query
    combine values into the ancestor (`ent.combine`), which take effect
    when the depth that sent them is through. So do the values it sends
    with `ent.apply` and `ent.accumulate`; the query may not read or set a
    field it sends to that way, where an entity of the same depth would
    see what others of it sent. What changes which entities there are, what
    they hold or the tree (`ent.despawn`, a moving `ent.add` or
    `ent.remove`, `ent.connect`, `ent.disconnect`) takes effect when the
    whole query has run, as in any query: every entity is visited as the
    tree was when it started.

    A cascading query may be reactive, and a `changed` trigger of it may be
    `up` the tree (`on [changed @Local, changed @World "x" up @Under]`):
    the event of the ancestor that a `!ent.ref<@World, up @Under>` argument
    leads to, read from the ancestor's stamp, so that what the query wrote
    to an ancestor is an event for the entities below it in the same run.
    `cascade @R bfs` and `cascade @R dfs` (also after `leaves first`) ask
    for the order exactly: depth by depth, or an entity and then all that
    is below it (`leaves first`: all that is below it, then the entity).
    Where that is not how the tree is stored it is worked out from the
    tree when the query runs.

    `changed @C before @R` is the same for the sibling a ref `before @R`
    leads to (and fires where the entity has another sibling before it
    than it had), and `changed @C down @R`, in a query cascading `leaves
    first`, for any child of the entity (and where it gained or lost one).
    Those events it has then passed down; they are none the next time it
    runs. An entity that is connected along the tree has such an event as
    well: the ancestor it sees is another. Over an unsorted tree, or one sorted in one archetype, the query
    goes only where its events lead (see
    WorldLayout::cascadeFollowsEvents); otherwise through
    its whole tree, running its body where a trigger fired.

    `by @C "f"` orders the query by a whole-number field of a component
    its entities have: they are visited one after another, the one with
    the smallest value first (those with the same value in the storage's
    order), by the values the field had when the query started. It is
    still one query: what it sends (`ent.apply`, `ent.accumulate`,
    `ent.append`) is combined in that order, and so it may not read or
    set a field it sends to, read a unique it accumulates into, or read a
    buffer it appends to.

    `by @C "f" turn` runs it as one query per entity, each with its own
    commit point:
    ```mlir
    ent.query (%m: !ent.ref<@Mobj, mut>) by @Mobj "id" turn { ... }
    ```
    What an entity's body sends, accumulates, writes to a resource
    (`ent.write`) or does to a buffer takes effect at once, and what it
    reads of other entities, resources and buffers is as it is then, with
    all that the entities before it did. Despawns, moves and connects take
    effect when that entity's body ends. An entity the body spawns is
    visited by the same query if its value of the field is not below the
    one of the entity that spawned it. Such a query is neither reactive
    nor along a tree.

    A query that goes by a field may visit a part of that order, its
    slice: the entities whose value is from one (`from`) up to below
    another (`to`), and of those the first or the last so many (`first`,
    `last`, an i32), in the order still:
    ```mlir
    ent.query (%c: !ent.ref<@Car>) by @Car "key" from %a : i64
        to %b : i64 first %k { ... }
    ```
    The values are worked out before it starts. Not with `turn`.

    A query whose `where` compares a field with a value that is the same
    for all its entities has that value as `equals` (`key_equals` names
    the field), which a hash index by the field may answer (`via_hash`):
    ```mlir
    ent.query (%p: !ent.ref<@Player>) equals %n : i64
        {key_equals = [@Player, "team"]} { ... }
    ```
  }];
  let arguments = (ins Variadic<AnyType>:$slice);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// Of a query that goes by a field: which of `from`, `to`, `first`
    /// and `last` its slice operands are, in their order.
    static constexpr ::llvm::StringLiteral kSliceAttr = "slice_of";
    /// The slice operand named `part`, or nothing.
    ::mlir::Value getSlicePart(::llvm::StringRef part);
    /// Of a query whose `where` compares a field of its entity's with a
    /// value that is the same for all of them: [component, field]; the
    /// value is the slice operand `equals` (a text's two hashes, as an
    /// `index ... hash` holds it, `equals_hash` and `equals_check`).
    static constexpr ::llvm::StringLiteral kKeyEqualsAttr = "key_equals";
    /// Set by --ent-indexes: the hash index by that field, over the same
    /// entities, that says which have that value; the query visits only
    /// those (still asking its `where`), in their order.
    static constexpr ::llvm::StringLiteral kViaHashAttr = "via_hash";
    ::mlir::FlatSymbolRefAttr getViaHash() {
      return (*this)->getAttrOfType<::mlir::FlatSymbolRefAttr>(kViaHashAttr);
    }
    /// Forgets the value a key is compared with (`key_equals`, and its
    /// slice operands), where no index says which entities have it.
    void dropEquals();
    /// Set by --ent-indexes: the sorted index by the same field, over the
    /// same entities, that the query takes its order from instead of
    /// sorting them (the same order).
    static constexpr ::llvm::StringLiteral kByIndexAttr = "by_index";
    ::mlir::FlatSymbolRefAttr getByIndex() {
      return (*this)->getAttrOfType<::mlir::FlatSymbolRefAttr>(kByIndexAttr);
    }
    /// The attribute holding a reactive query's triggers: an array of
    /// [kind, component, field] entries, the field empty unless the kind
    /// is "changed" with a field.
    static constexpr ::llvm::StringLiteral kTriggersAttr = "triggers";
    /// Filters: arrays of components (`any`: an array of such arrays).
    static constexpr ::llvm::StringLiteral kWithAttr = "with";
    static constexpr ::llvm::StringLiteral kWithoutAttr = "without";
    static constexpr ::llvm::StringLiteral kAnyAttr = "any";
    /// The tree relation the query is ordered by, parents first.
    static constexpr ::llvm::StringLiteral kCascadeAttr = "cascade";
    ::mlir::FlatSymbolRefAttr getCascade() {
      return (*this)->getAttrOfType<::mlir::FlatSymbolRefAttr>(kCascadeAttr);
    }
    /// With `cascade`: children before their parents.
    static constexpr ::llvm::StringLiteral kLeavesFirstAttr = "leaves_first";
    bool isLeavesFirst() { return (*this)->hasAttr(kLeavesFirstAttr); }
    /// With `cascade`: the order exactly, "bfs" (depth by depth) or "dfs"
    /// (an entity and all below it before the next; children first, all
    /// below an entity before it). Empty: any order in which parents
    /// come before their children, or after with `leaves first`.
    static constexpr ::llvm::StringLiteral kTraversalAttr = "traversal";
    ::llvm::StringRef getTraversal() {
      auto attr = (*this)->getAttrOfType<::mlir::StringAttr>(kTraversalAttr);
      return attr ? attr.getValue() : ::llvm::StringRef();
    }
    /// The field the query's entities are visited by, smallest first: a
    /// component the entity must have and a whole-number field of it.
    static constexpr ::llvm::StringLiteral kByAttr = "by";
    bool hasBy() { return (*this)->hasAttr(kByAttr); }
    ::mlir::FlatSymbolRefAttr getByComponent() {
      auto by = (*this)->getAttrOfType<::mlir::ArrayAttr>(kByAttr);
      return by && by.size() == 2
                 ? ::llvm::dyn_cast<::mlir::FlatSymbolRefAttr>(by[0])
                 : ::mlir::FlatSymbolRefAttr();
    }
    ::mlir::StringAttr getByField() {
      auto by = (*this)->getAttrOfType<::mlir::ArrayAttr>(kByAttr);
      return by && by.size() == 2
                 ? ::llvm::dyn_cast<::mlir::StringAttr>(by[1])
                 : ::mlir::StringAttr();
    }
    /// With `by`: one query per entity, each with its own commit point.
    static constexpr ::llvm::StringLiteral kTurnAttr = "turn";
    bool isTurn() { return (*this)->hasAttr(kTurnAttr); }
    /// Whether an argument is a ref to an ancestor's component.
    bool hasUpRefs();

    /// The components the entity must have: those bound (its own), then
    /// `with`.
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

def Ent_BoundOp : Ent_Op<"bound"> {
  let summary = "Whether an optional ref leads to an entity";
  let description = [{
    ```mlir
    %there = ent.bound %prev : !ent.ref<@Offset, before @Inside, optional>
    ```
    True if the visited entity has the ancestor or the sibling before it
    that the ref, an `optional` one up or before a tree, stands for.
  }];
  let arguments = (ins Ent_RefType:$ref);
  let results = (outs I1:$result);
  let assemblyFormat = "$ref attr-dict `:` type($ref)";
  let hasVerifier = 1;
}

def Ent_SameOp : Ent_Op<"same", [Pure]> {
  let summary = "Whether two ids are of one entity";
  let description = [{
    ```mlir
    %is = ent.same %a, %b
    ```
    An entity's id is not a number to compute with; this compares two.
  }];
  let arguments = (ins Ent_EntityType:$lhs, Ent_EntityType:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict";
}

def Ent_HashOp : Ent_Op<"hash", [Pure]> {
  let summary = "A hash of a value: its bits mixed into 64";
  let description = [{
    ```mlir
    %h = ent.hash %v : !ent.entity
    ```
    Of a whole number, an enum, an entity or a row number: its bits as
    stored, mixed by one 64 by 64 bit multiply to 128 bits, the halves
    xored (rapidhash's `rapid_mix`, with its secrets). The same on every
    machine and every run. (`hash_of(text)` is a loop of its own.)
  }];
  let arguments = (ins AnyType:$value);
  let results = (outs I64:$result);
  let assemblyFormat = "$value attr-dict `:` type($value)";
}

def Ent_IndexDuplicateOp : Ent_Op<"index.duplicate"> {
  let summary = "Stop the program: two entities of a unique index's key";
  let description = [{
    ```mlir
    ent.index.duplicate @ById(%key, %a, %b) : i64
    ```
    In what a `unique` hash index declares, where it finds a second entity
    of a key: the program says so, the key (of a whole number) and both
    entities, and stops.
  }];
  let arguments = (ins FlatSymbolRefAttr:$index, AnyType:$key, AnyType:$a,
      AnyType:$b);
  let assemblyFormat = "$index `(` $key `,` $a `,` $b `)` attr-dict `:` "
                       "type($key) `,` type($a) `,` type($b)";
}

def Ent_TextConstantOp : Ent_Op<"text.constant", [Pure]> {
  let summary = "A text of any length that is written in the program";
  let description = [{
    ```mlir
    %t = ent.text.constant "some words"
    ```
    A view of bytes that are the program's own, and never change.
  }];
  let arguments = (ins StrAttr:$value);
  let results = (outs Ent_StringType:$result);
  let assemblyFormat = "$value attr-dict";
}

def Ent_TextOfOp : Ent_Op<"text.of", [Pure]> {
  let summary = "A text of a capacity as one of any length";
  let description = [{
    ```mlir
    %t = ent.text.of %fixed : !ent.text<30>
    ```
    A view of the bytes `%fixed` holds, which are kept for as long as the
    function it is in runs.
  }];
  let arguments = (ins Ent_TextType:$value);
  let results = (outs Ent_StringType:$result);
  let assemblyFormat = "$value attr-dict `:` type($value)";
}

def Ent_TextCutOp : Ent_Op<"text.cut", [NoMemoryEffect]> {
  let summary = "A text of any length as one of a capacity";
  let description = [{
    ```mlir
    %fixed = ent.text.cut %t : !ent.text<30>
    ```
    The bytes of `%t`, as many of them as the capacity holds.

    (Like the other ops that look into a text of any length, it is not
    run where its value is not asked for: a field of a component an
    entity lacks holds no address to look at.)
  }];
  let arguments = (ins Ent_StringType:$value);
  let results = (outs Ent_TextType:$result);
  let assemblyFormat = "$value attr-dict `:` type($result)";
}

def Ent_TextLengthOp : Ent_Op<"text.length", [NoMemoryEffect]> {
  let summary = "How many bytes a text of any length has";
  let description = [{
    ```mlir
    %n = ent.text.length %t
    ```
  }];
  let arguments = (ins Ent_StringType:$value);
  let results = (outs I32:$result);
  let assemblyFormat = "$value attr-dict";
}

def Ent_TextAtOp : Ent_Op<"text.at", [NoMemoryEffect]> {
  let summary = "A byte of a text of any length";
  let description = [{
    ```mlir
    %b = ent.text.at %t, %i : i32
    ```
    Byte `%i` of `%t`; 0 where there is none.
  }];
  let arguments = (ins Ent_StringType:$value, AnySignlessInteger:$index);
  let results = (outs I8:$result);
  let assemblyFormat = "$value `,` $index attr-dict `:` type($index)";
}

def Ent_TextHashOp : Ent_Op<"text.hash", [NoMemoryEffect]> {
  let summary = "A hash of a text of any length (see `hash_of`)";
  let description = [{
    ```mlir
    %h = ent.text.hash %t, %seed
    ```
    The text's bytes 16 at a time, two words of 8 (little-endian, 0 past
    its end), each block mixed into the hash (from `%seed`) as rapidhash
    mixes, then with its length (devices/text.c, `ent_text_hash`).
  }];
  let arguments = (ins Ent_StringType:$value, I64:$seed);
  let results = (outs I64:$result);
  let assemblyFormat = "$value `,` $seed attr-dict";
}

def Ent_TextEqualOp : Ent_Op<"text.equal", [NoMemoryEffect]> {
  let summary = "Whether two texts of any length have the same bytes";
  let description = [{
    ```mlir
    %is = ent.text.equal %a, %b
    ```
  }];
  let arguments = (ins Ent_StringType:$lhs, Ent_StringType:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict";
}

def Ent_TextJoinOp : Ent_Op<"text.join", [NoMemoryEffect]> {
  let summary = "Two texts of any length as one";
  let description = [{
    ```mlir
    %both = ent.text.join %a, %b
    ```
    A view of a block with the bytes of `%a` and then those of `%b`,
    which is nobody's: it is kept until the schedule that runs is done,
    so the view is good wherever it goes meanwhile, and a field that is
    to hold it takes a copy like any other (`ent.text.own`).
  }];
  let arguments = (ins Ent_StringType:$lhs, Ent_StringType:$rhs);
  let results = (outs Ent_StringType:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict";
}

def Ent_TextKeepOp : Ent_Op<"text.keep", [NoMemoryEffect]> {
  let summary = "A copy of a text that is kept while the schedule runs";
  let description = [{
    ```mlir
    %kept = ent.text.keep %t
    ```
    A view of a block with the bytes of `%t`, kept like one that
    `ent.text.join` makes: for a text whose own bytes do not stay (those
    of a text of a capacity that a fn gives back as one of any length).
  }];
  let arguments = (ins Ent_StringType:$value);
  let results = (outs Ent_StringType:$result);
  let assemblyFormat = "$value attr-dict";
}

def Ent_TextOwnOp : Ent_Op<"text.own", [
    MemoryEffects<[MemRead, MemWrite]>]> {
  let summary = "A block of its own with the bytes of a text";
  let description = [{
    ```mlir
    %held = ent.text.own %t
    ```
    What a field is given to hold: a copy of the bytes of `%t` (nothing,
    the address 0, for none), which is the field's until `ent.text.drop`
    takes it.
  }];
  let arguments = (ins Ent_StringType:$value);
  let results = (outs Ent_StringType:$result);
  let assemblyFormat = "$value attr-dict";
}

def Ent_TextDropOp : Ent_Op<"text.drop", [
    MemoryEffects<[MemRead, MemWrite]>]> {
  let summary = "Give back the block a field held";
  let description = [{
    ```mlir
    ent.text.drop %held
    ```
    `%held` is what a field holds that is given another text, or that
    goes with its entity: no view of it is used after this.
  }];
  let arguments = (ins Ent_StringType:$value);
  let assemblyFormat = "$value attr-dict";
}

def Ent_TableOp : Ent_Op<"table", [Symbol]> {
  let summary = "Declare a table: rows of values that are the program's own";
  let description = [{
    ```mlir
    "ent.table"() {sym_name = "road", field_names = ["x", "y"],
                   field_types = [f32, f32],
                   values = [[-1.0 : f32, 4.0 : f32], [2.0 : f32, 2.0 : f32]]}
    ```
    A list of rows that never changes, each with a value for every field
    (`values` holds them field by field, as they are stored: a number as
    it is, an enum's case as its byte, a text as its integer). A plain
    list has one field without a name. `ent.table.at` reads it. With
    `key` it has a row for each case of that enum, in their order, and is
    read by the number of a case.
  }];
  let arguments = (ins SymbolNameAttr:$sym_name, StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types, ArrayAttr:$values,
                       OptionalAttr<FlatSymbolRefAttr>:$key);
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// How many rows it has.
    unsigned getNumRows() {
      return ::llvm::cast<::mlir::ArrayAttr>(getValues()[0]).size();
    }
    /// The place of the field `name` among its fields, if it has one.
    std::optional<unsigned> findField(::llvm::StringRef name) {
      for (auto [index, field] : ::llvm::enumerate(
               getFieldNames().getAsValueRange<::mlir::StringAttr>()))
        if (field == name)
          return index;
      return std::nullopt;
    }
  }];
}

def Ent_TableAtOp : Ent_Op<"table.at", [
    Pure, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "A value of a row of a table";
  let description = [{
    ```mlir
    %x = ent.table.at @road "x"[%i : i32] : f32
    ```
    What row `%i` has in that field; nought (the first case, an empty
    text) where the table has no such row.
  }];
  let arguments = (ins FlatSymbolRefAttr:$table, StrAttr:$field,
                       AnySignlessInteger:$index);
  let results = (outs AnyType:$result);
  let assemblyFormat = [{
    $table $field `[` $index `:` type($index) `]` attr-dict `:` type($result)
  }];
}

def Ent_NobodyOp : Ent_Op<"nobody", [Pure]> {
  let summary = "The id of no entity";
  let description = [{
    ```mlir
    %none = ent.nobody
    ```
    An id that is no entity's, and never will be (`none` in a program):
    for a value that is to say which entity before one is found. It is
    the same as itself (`ent.same`) and as no other id; what is sent to
    it goes nowhere, and what is looked up of it is not found.
  }];
  let results = (outs Ent_EntityType:$result);
  let assemblyFormat = "attr-dict";
}

def Ent_OtherOp : Ent_Op<"other"> {
  let summary = "The entity a ref to another entity's component leads to";
  let description = [{
    ```mlir
    %parent = ent.other %outer : !ent.ref<@Box, parent @Inside>
    ```
    The id of the ancestor or the sibling that the ref, one up, before or
    after a tree, stands for; where an `optional` ref leads nowhere, the
    id of no entity.
  }];
  let arguments = (ins Ent_RefType:$ref);
  let results = (outs Ent_EntityType:$result);
  let assemblyFormat = "$ref attr-dict `:` type($ref)";
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

    Values may go round, as in `ent.each`: the block's arguments after the
    two are values that start as the op's operands (`iter`), are in every
    run what the run before yielded, and what the last run yields is the
    op's result: the visited entity's own, so an edge loop adds into a
    `var` of the body around it, and into a resource by way of one.
  }];
  let arguments = (ins FlatSymbolRefAttr:$relation, StrAttr:$direction,
                       Variadic<AnyType>:$inits);
  let results = (outs Variadic<AnyType>:$results);
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    bool isOut() { return getDirection() == "out"; }
  }];
}

def Ent_EachOp : Ent_Op<"each", [
    SingleBlockImplicitTerminator<"YieldOp">, RecursiveMemoryEffects]> {
  let summary = "Run the body for every entity of a kind, for the visited one";
  let description = [{
    ```mlir
    %nearest = "ent.each"(%far) ({
    ^bb0(%p: !ent.ref<@Place>, %e: !ent.entity, %best: f32):
      %x = ent.get %p "x" : !ent.ref<@Place> -> f32
      ...
      ent.yield %less : f32
    }) {entity, without = [@Hidden]} : (f32) -> f32
    ```
    A `for` over entities inside the body of an `ent.query`: for the
    entity the query visits, the body runs once for every entity that has
    the components of the refs (the block's first arguments) and of
    `with`, and none of `without`, one after another in a fixed order (by
    archetype, then row). With `entity` the argument after the refs is
    that entity's id. The remaining arguments are values that go round:
    each starts as the operand of that place, is in every run what the
    run before yielded, and what the last run yields is the op's result.

    The body only reads those entities (`ent.get` through the refs, which
    are not `mut`), and never a field the enclosing query sets of its own
    entities, of which some would be set already and some not; unless the
    ref is one of `old` (the numbers of such refs), which reads what the
    field was when the query started, of which a copy is kept for as long
    as the query runs. It may read and write what the enclosing query
    binds. What it sends (`ent.apply`) or accumulates is combined right
    away, once for each of its entities, so nothing else in the query
    reads or sets that field. Nothing in it changes what entities there
    are.
  }];
  let arguments = (ins Variadic<AnyType>:$inits,
                       OptionalAttr<ArrayAttr>:$with,
                       OptionalAttr<ArrayAttr>:$without, UnitAttr:$entity,
                       OptionalAttr<I64ArrayAttr>:$old);
  let results = (outs Variadic<AnyType>:$results);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The refs: the block's leading arguments.
    unsigned getNumRefs() {
      unsigned count = 0;
      for (::mlir::BlockArgument arg : getBody().front().getArguments()) {
        if (!::llvm::isa<RefType>(arg.getType()))
          break;
        ++count;
      }
      return count;
    }
    /// The components an entity has to have, and those it must not.
    ::llvm::SmallVector<::mlir::FlatSymbolRefAttr> getRequired();
    ::llvm::SmallVector<::mlir::FlatSymbolRefAttr> getExcluded();
    /// Whether the ref of that number reads what was before the query.
    bool isOld(unsigned ref) {
      if (std::optional<::mlir::ArrayAttr> old = getOld())
        for (::mlir::Attribute number : *old)
          if (::llvm::cast<::mlir::IntegerAttr>(number).getInt() ==
              int64_t(ref))
            return true;
      return false;
    }
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
    declaration order. Outside a query it is there for every query that
    follows (the edges are sorted before the system's next query and when
    the system ends). Inside one
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

def Ent_CombineOp : Ent_Op<"combine", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Combine a value into a field of an ancestor, through a ref";
  let description = [{
    ```mlir
    ent.combine %down "flow" add %own : !ent.ref<@Node, mut, up @Flows>, f32
    ```
    Combines `%own` into field `flow` of the component the ref leads to, of
    the visited entity's ancestor, with the rule `add`, `min` or `max`
    (signed for integers). The enclosing query cascades along the ref's
    tree, and the values take effect when the depth that sent them is
    through: `field ⊕ v1 ⊕ v2 ⊕ ...` over the values the entities of that
    depth sent to the ancestor, in the order the query visits them, which
    is fixed. So with `leaves first` an entity that reads the field of its
    own has there what all below it sent.

    The query may not read the field through a ref up the tree or look it
    up: entities of one depth would see what others of it sent. All its
    combines into one field use the same rule, and the field is an integer
    (not `i1`) or a float. A system that declares its access must list the
    component in `writes`.
  }];
  let arguments = (ins Ent_RefType:$ref, StrAttr:$field, StrAttr:$rule,
                       AnyType:$value);
  let assemblyFormat = [{
    $ref $field custom<Rule>($rule) $value attr-dict
    `:` type($ref) `,` type($value)
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
    `min` or `max` (signed for integers), or `set`: the last value sent is
    the field's. A resource cannot be written inside a query, where every
    entity would write the same field; an accumulate is the alternative
    that does not depend on how the query ran. Like `ent.apply`, its values
    are combined when the query ends, in a fixed order (by accumulate, then
    archetype, then row), so the result does not depend on how the query
    ran, in parallel or not, even for floating-point `add`; reads of the
    resource in the same query see the value it had when the query started.

    A system that declares its access must list the resource in `writes`. An
    accumulate must be inside a query and not inside a loop there, all
    accumulates of one query to the same field use the same rule, and the field
    must be an integer (not `i1`) or a float; `set` also takes an `i1`, an
    enum and an entity.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field,
                       StrAttr:$rule, AnyType:$value);
  let assemblyFormat = [{
    $resource $field custom<Rule>($rule) $value attr-dict `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ent_RunningOp : Ent_Op<"running", [
    DeclareOpInterfaceMethods<SymbolUserOpInterface>,
    AllTypesMatch<["value", "before"]>]> {
  let summary = "Accumulate into a resource field, and give what it came to "
                "before";
  let description = [{
    ```mlir
    %first = ent.running @Laying "stubs" add %meets : i32
    ```
    Like `ent.accumulate` (`add`, `min` or `max`, of whole numbers), and
    gives what the field had come to before this entity's value: the
    field's value when the query started combined with the values of the
    entities before this one, in the query's order (by archetype, then
    row). So `%first` is a running sum, the same however the query ran:
    one after another, or in parallel in two passes (the values first,
    then their running sums, then the body). The field ends with all of
    them combined.

    In the body of a query (not a loop of it, nor a `for` over other
    entities or edges, nor one that goes by a field, along a tree or
    reacts to events); nothing else in the query reads the field or
    accumulates into it.
  }];
  let arguments = (ins FlatSymbolRefAttr:$resource, StrAttr:$field,
                       StrAttr:$rule, AnyType:$value);
  let results = (outs AnyType:$before);
  let assemblyFormat = [{
    $resource $field custom<Rule>($rule) $value attr-dict `:` type($value)
  }];
  let hasVerifier = 1;
}

def Ent_RowsCheckOp : Ent_Op<"rows_check"> {
  let summary = "Say where an entity of a query wrote outside its rows";
  let description = [{
    ```mlir
    ent.rows_check %inside @Index
    ```
    In a query that writes rows of `@Index`: where `%inside` is false, the
    entity would write a row that is not its own (outside the interval a
    running sum gives it, or with a running sum that went down, which
    makes those of others overlap); the query's end then stops the
    program. Made by the lowering from what the verifier could not show
    (see ownRowOf); the write itself is not made.
  }];
  let arguments = (ins I1:$inside, FlatSymbolRefAttr:$buffer);
  let assemblyFormat = "$inside $buffer attr-dict";
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
  let summary = "Call a function over values";
  let description = [{
    ```mlir
    %h = ent.invoke @hash(%id) : (i64) -> i64
    ent.invoke proc @put(%line) : (!ent.text<126>) -> ()
    %b, %a = ent.invoke @swap(%x, %y) : (i32, f32) -> (f32, i32)
    ```
    Calls an `ent.function` from inside a system, or from the body of
    another function (which calls no proc). `proc` repeats what the
    function declares: the call then has effects outside the world, so its
    system keeps its place among all others and a query holding it visits
    its entities in order, one at a time.
  }];
  let arguments = (ins FlatSymbolRefAttr:$callee, Variadic<AnyType>:$args,
                       UnitAttr:$proc);
  let results = (outs Variadic<AnyType>:$results);
  let assemblyFormat = [{
    (`proc` $proc^)? $callee `(` $args `)` attr-dict
    `:` functional-type($args, $results)
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The value the call gives, if any: the first of several.
    ::mlir::Value getResult() {
      return getNumResults() ? getOperation()->getResult(0) : ::mlir::Value();
    }
  }];
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
                 "EdgesOp", "MainOp", "LoopOp", "FunctionOp", "EachOp"]>]> {
  let summary = "Region terminator; implicit in the custom syntax";
  let description = [{
    Ends a body without operands, and a condition or a function with its
    value.
  }];
  let arguments = (ins Variadic<AnyType>:$results);
  let builders = [OpBuilder<(ins), [{ /* nothing to do */ }]>];
  let assemblyFormat = "attr-dict ($results^ `:` type($results))?";
}

//===----------------------------------------------------------------------===//
// State and access (docs: "explicit state and access in the lowering")
//===----------------------------------------------------------------------===//
//
// What a loop of the lowering touches, said as ops between the `ent` ops
// and the loops they become, so that how a loop runs in parallel is
// decided by its accesses (resolveStateAccess in LowerToLoops.cpp) and
// carried out by a lowering per backend. So far: what an apply or an
// accumulate in a query sends, and the edges and rows it adds, made and
// lowered where they are sent and where the query ends.

def Ent_SaCombineOp : Ent_Op<"sa.combine", [AttrSizedOperandSegments]> {
  let summary = "Combine into another entity's field: an access of a loop";
  let description = [{
    ```mlir
    ent.sa.combine 2[1] @Cell "got" "add" (%e, %v) at %row if %mask
      in %world {resolution = "rows"} : !ent.entity, i32, memref<4096xi8>
    ```
    What apply 2 of the layout (`ent.apply`; -1, one that has no
    buffers there), in the body of a query over archetype 1, sends: `%v` combined into field `got` of `@Cell` of
    the entity `%e` by the rule `add`, `min` or `max`. `%mask`, where the
    body runs only for some entities, says whether it does here; `%tick`
    is the world's tick where it has stamps; `%at` its row, or its edge's
    position in a loop over edges.

    What it is, which decides how it is done (resolveStateAccess in
    LowerToLoops.cpp): `atomic` (whole numbers for the GPU), `looped`
    (from a loop of the body, once for each of its turns), `unobserved`
    (nothing else in the query touches the field), `in_each` (in a query
    visiting entities one after another in a fixed order), `order_free`
    (whole numbers of a reactive query), `direct` (the query's loop may
    combine where it is sent), `sliced` (in a slice of a parallel loop),
    `in_edges` (in a loop over edges), `trusted` (its id is alive);
    on many ranks, `posts` (in a query over distributed archetypes: one
    sent to another rank's entity is posted to it), `whole` (a whole
    number, stored), `changed` (combining it stamps a Changed event).

    The decision, `resolution`: `"atomic"` or `"direct"`, combined where it
    is sent; `"list"`, staged on the list of the slice of the loop it is
    in (the first, on one thread) until the query ends (devices/sends.c, ent_sends_put);
    `"rows"` or `"edges"`, in a slot of the row or the edge `%at`, until
    the query ends. Where it is staged, an `ent.sa.merge` of the same
    apply and archetype combines. And on many ranks, `on_ranks`:
    `"posted"` (to the rank that has the entity, combined there when the
    query ends; whole numbers added, or kept the least or the most, the
    same in any order) or `"refused"` (the program stops where one would
    be posted); see ranksPosting.
  }];
  let arguments = (ins I64Attr:$apply, I64Attr:$archetype,
                       FlatSymbolRefAttr:$component, StrAttr:$field,
                       StrAttr:$rule, AnyType:$entity, AnyType:$value,
                       Optional<Index>:$at, Optional<I1>:$mask,
                       Optional<I64>:$tick, AnyMemRef:$world,
                       UnitAttr:$atomic, UnitAttr:$looped,
                       UnitAttr:$unobserved, UnitAttr:$in_each,
                       UnitAttr:$order_free, UnitAttr:$direct,
                       UnitAttr:$sliced, UnitAttr:$in_edges,
                       UnitAttr:$trusted, UnitAttr:$posts, UnitAttr:$whole,
                       UnitAttr:$changed, OptionalAttr<StrAttr>:$resolution,
                       OptionalAttr<StrAttr>:$on_ranks);
  let assemblyFormat = [{
    $apply `[` $archetype `]` $component $field $rule `(` $entity `,` $value
    `)` (`at` $at^)? (`if` $mask^)? (`tick` $tick^)? `in` $world attr-dict
    `:` type($entity) `,` type($value) `,` type($world)
  }];
}

def Ent_SaMergeOp : Ent_Op<"sa.merge", [AttrSizedOperandSegments]> {
  let summary = "Combine what was staged for an apply into its fields";
  let description = [{
    ```mlir
    ent.sa.merge 2[1] @Cell "got" "add" of i32 in %world tick %tick
      {least = 16384 : i64, parallel, resolution = "groups",
       staged = "list"} : memref<4096xi8>
    ```
    Where the query ends: what `ent.sa.combine` of apply 2 staged for
    the entities of archetype 1, as `staged` says (`"list"`, `"rows"` or
    `"edges"`), combined into the field, each entity's values in the order
    they were staged (row by row or slice by slice, then as sent), so the
    same on any number of threads. `%count` is how many rows the query
    went through (rows and edges), on many ranks from `%first`, the first
    of this rank's; `%tick` the world's tick where it has stamps. For edges, `relation` and `inward` say which loop it was in.

    What decides how (resolveStateAccess): `parallel` (the loop may have
    run on many threads), `order_shows` (combining it stamps a Changed
    event or reorders a tree, so the order of entities shows), `atomic`,
    `ranks` (built for many ranks). The decision, `resolution`: for a
    list, `"in-order"` (one value after another) or `"groups"` (in groups
    of the entities sent to, on many threads, where that takes less:
    chosen as the program runs, both giving the same); `"rows"` and
    `"edges"`, row after row (and edge after edge). On many ranks, from
    `posts`, `whole` and `changed`, `on_ranks` as for `ent.sa.combine`.
  }];
  let arguments = (ins I64Attr:$apply, I64Attr:$archetype,
                       FlatSymbolRefAttr:$component, StrAttr:$field,
                       StrAttr:$rule, TypeAttr:$stored, StrAttr:$staged,
                       AnyMemRef:$world, Optional<Index>:$count,
                       Optional<Index>:$first, Optional<I64>:$tick,
                       OptionalAttr<FlatSymbolRefAttr>:$relation,
                       UnitAttr:$inward, UnitAttr:$parallel,
                       UnitAttr:$order_shows, UnitAttr:$atomic,
                       UnitAttr:$trusted, UnitAttr:$ranks, UnitAttr:$posts,
                       UnitAttr:$whole, UnitAttr:$changed, I64Attr:$least,
                       OptionalAttr<StrAttr>:$resolution,
                       OptionalAttr<StrAttr>:$on_ranks);
  let assemblyFormat = [{
    $apply `[` $archetype `]` $component $field $rule `of` $stored `in`
    $world (`count` $count^)? (`from` $first^)? (`tick` $tick^)? attr-dict
    `:` type($world)
  }];
}

def Ent_SaAppendOp : Ent_Op<"sa.append", [AttrSizedOperandSegments]> {
  let summary = "Append a row or an edge: an access of a loop";
  let description = [{
    ```mlir
    ent.sa.append "connect" 4[1] @ToldBy (%a, %b, %day) at %row
      in %world {looped, resolution = "list", sliced}
      : (!ent.entity, !ent.entity, i32), memref<4096xi8>
    ```
    What connect 4 of the layout (`ent.connect`; `"buffer"`, append 4,
    `ent.append`), in the body of a query over archetype 1, adds: an edge
    of `@ToldBy` from the first value to the second, with the rest for its
    fields (a row of the buffer, the values its fields). `%at` is its
    row; `%mask`, where the body runs only for some entities, whether it
    does here.

    What it is: `looped` (from a loop of the body, or a loop over edges
    there: once for each of its turns), `sliced` (in a slice of a
    parallel loop). The decision (resolveStateAccess), `resolution`:
    `"list"`, staged on the list of the slice of the loop it is in (the
    ends, then the values two by two; devices/sends.c), or `"rows"`, in
    its row's places; an `ent.sa.place` of the same op and archetype adds
    them where the query ends.
  }];
  let arguments = (ins StrAttr:$kind, I64Attr:$index, I64Attr:$archetype,
                       FlatSymbolRefAttr:$target, Variadic<AnyType>:$values,
                       Optional<Index>:$at, Optional<I1>:$mask,
                       AnyMemRef:$world, UnitAttr:$looped, UnitAttr:$sliced,
                       OptionalAttr<StrAttr>:$resolution);
  let assemblyFormat = [{
    $kind $index `[` $archetype `]` $target `(` $values `)` (`at` $at^)?
    (`if` $mask^)? `in` $world attr-dict `:` `(` type($values) `)` `,`
    type($world)
  }];
}

def Ent_SaPlaceOp : Ent_Op<"sa.place", [AttrSizedOperandSegments]> {
  let summary = "Add what was staged for a connect or an append";
  let description = [{
    ```mlir
    ent.sa.place "connect" 4[1] @ToldBy in %world
      {least = 1024 : i64, parallel, resolution = "reserved",
       staged = "list"} : memref<4096xi8>
    ```
    Where the query ends: what `ent.sa.append` of connect 4 (or append 4,
    `"buffer"`) staged for the entities of archetype 1, as `staged` says
    (`"list"` or `"rows"`, the first `%count` rows; on many ranks from
    `%first`, the first of this rank's), added at the end of the
    relation's table (the buffer's rows) in the order it was staged: row
    by row, or slice by slice and then as sent.

    What decides how (resolveStateAccess): `parallel` (the loop may have
    run on many threads), `ranks` (the rows are this rank's of a
    distributed archetype), and what adding an edge does besides writing
    it at the end of the table: `partitions` (built for partitions, an
    edge kept where an end is), `stamps` (it stamps the tick its entity
    was connected at), `linked`, `ordered`, `tree` (it links the edge, or
    orders a tree). The decision, `resolution`: for a list, `"reserved"`
    (from `least` edges on, a range of the table reserved at once and each
    slice's edges written in its own on many threads), `"routed"` (sent to
    the ranks of their ends, which add them) or `"in-order"` (one edge
    after another); for rows, `"rows"` (row after row), and on many ranks
    `"routed"` (an edge sent to the ranks of its ends) or `"shared"` (a
    buffer's rows after those of the ranks before, given to every rank).
    What differs on many ranks is their lowering (ranksAddEdge,
    ranksRowsBefore, ranksShareRows in LowerToLoops.cpp).
  }];
  let arguments = (ins StrAttr:$kind, I64Attr:$index, I64Attr:$archetype,
                       FlatSymbolRefAttr:$target, StrAttr:$staged,
                       AnyMemRef:$world, Optional<Index>:$count,
                       Optional<Index>:$first,
                       UnitAttr:$parallel, UnitAttr:$ranks,
                       UnitAttr:$partitions, UnitAttr:$stamps,
                       UnitAttr:$linked, UnitAttr:$ordered, UnitAttr:$tree,
                       I64Attr:$least, OptionalAttr<StrAttr>:$resolution);
  let assemblyFormat = [{
    $kind $index `[` $archetype `]` $target `in` $world
    (`count` $count^)? (`from` $first^)? attr-dict `:` type($world)
  }];
}

def Ent_SaAccumulateOp : Ent_Op<"sa.accumulate", [AttrSizedOperandSegments]> {
  let summary = "Accumulate into a unique's field: an access of a loop";
  let description = [{
    ```mlir
    ent.sa.accumulate 5[1] @Score "points" "add" (%v) at %row in %world
      {reduced, resolution = "reduce", unobserved} : i32, memref<4096xi8>
    ```
    What accumulate 5 of the layout (`ent.accumulate`), in the body of a
    query over archetype 1, sends: `%v` combined into field `points` of
    the unique `@Score` by the rule `add`, `min`, `max` or `set`. `%at` is
    its row; `%mask`, where the body runs only for some entities, whether
    it does here.

    What it is, as for `ent.sa.combine` (`atomic`, `looped`,
    `unobserved`, `in_each`, `order_free`, `direct`, `sliced`), and
    `reduced`: a whole number nothing else in the query sees, sent from
    the body itself or an `if` of it, in a query that may run in parallel.
    The decision (resolveStateAccess), `resolution`: `"atomic"` (for the
    GPU, into its copy's cell) or `"direct"`, combined where it is sent;
    `"reduce"`, the same, and a parallel loop over the entities reduces
    the cell (each thread from the number that changes nothing, then the
    threads'; see keepCells), a loop on one thread keeps it in a
    register; `"list"`, staged on the list of the slice of the loop it is
    in; `"rows"`, in a slot of its row. Where it is staged, an
    `ent.sa.fold` of the same accumulate and archetype combines.
  }];
  let arguments = (ins I64Attr:$apply, I64Attr:$archetype,
                       FlatSymbolRefAttr:$resource, StrAttr:$field,
                       StrAttr:$rule, AnyType:$value, Optional<Index>:$at,
                       Optional<I1>:$mask, AnyMemRef:$world,
                       UnitAttr:$atomic, UnitAttr:$looped,
                       UnitAttr:$unobserved, UnitAttr:$in_each,
                       UnitAttr:$order_free, UnitAttr:$reduced,
                       UnitAttr:$direct, UnitAttr:$sliced,
                       OptionalAttr<StrAttr>:$resolution);
  let assemblyFormat = [{
    $apply `[` $archetype `]` $resource $field $rule `(` $value `)`
    (`at` $at^)? (`if` $mask^)? `in` $world attr-dict `:` type($value) `,`
    type($world)
  }];
}

def Ent_SaFoldOp : Ent_Op<"sa.fold", [AttrSizedOperandSegments]> {
  let summary = "Fold what was staged for an accumulate into its unique";
  let description = [{
    ```mlir
    ent.sa.fold 5[1] @Score "points" "add" of i32 in %world count %n
      {resolution = "rows", staged = "rows"} : memref<4096xi8>
    ```
    Where the query ends: what `ent.sa.accumulate` of accumulate 5 staged
    for the entities of archetype 1, as `staged` says (`"list"`, or
    `"rows"`, the first `%count` rows, on many ranks from `%first`),
    folded into the unique's field in
    the order it was staged, so the result does not depend on how the
    query's loop ran. Or `"cell"`, apply and archetype -1: on the GPU,
    what its loops accumulated atomically, whole numbers, into the cell
    of its copy of the world at the unique's place (the same in any
    order). `no_sign`: a whole number without a sign. The decision,
    `resolution`: `"in-order"` for a list (slice after slice, then as
    sent), `"rows"` row after row (rows that sent nothing are skipped),
    `"cell"` the cell read back and combined into the CPU's unique.
  }];
  let arguments = (ins I64Attr:$apply, I64Attr:$archetype,
                       FlatSymbolRefAttr:$resource, StrAttr:$field,
                       StrAttr:$rule, TypeAttr:$stored, StrAttr:$staged,
                       AnyMemRef:$world, Optional<Index>:$count,
                       Optional<Index>:$first, UnitAttr:$no_sign,
                       OptionalAttr<StrAttr>:$resolution);
  let assemblyFormat = [{
    $apply `[` $archetype `]` $resource $field $rule `of` $stored `in`
    $world (`count` $count^)? (`from` $first^)? attr-dict `:` type($world)
  }];
}

#endif // ENT_OPS
