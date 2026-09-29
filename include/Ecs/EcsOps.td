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
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       FlatSymbolRefArrayAttr:$components,
                       ConfinedAttr<I64Attr, [IntPositive]>:$capacity);
  let assemblyFormat = [{
    $sym_name custom<SymbolList>($components) `capacity` $capacity attr-dict
  }];
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    bool contains(::mlir::FlatSymbolRefAttr component);
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
    `writes` implies read access. Entry block arguments are the system's
    per-run parameters.
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
  }];
  let regions = (region SizedRegion<1>:$body);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
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
