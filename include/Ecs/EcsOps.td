#ifndef ECS_OPS
#define ECS_OPS

include "Ecs/EcsDialect.td"
include "Ecs/EcsTypes.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

def Ecs_ComponentOp : Ecs_Op<"component", [Symbol]> {
  let summary = "Declare a component: an ordered list of scalar fields";
  let description = [{
    ```mlir
    ecs.component @Position (x: f32, y: f32)
    ```
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       StrArrayAttr:$field_names,
                       TypeArrayAttr:$field_types);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The type of field `name`, or null if the component has no such field.
    ::mlir::Type getFieldType(::llvm::StringRef name);
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
    Each entry block argument is an `!ecs.ref` to a distinct component.
    Iteration order is unspecified, so the body must not depend on it.
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

def Ecs_RunOp : Ecs_Op<"run", [
    HasParent<"ScheduleOp">,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "Run a system as one step of a schedule";
  let arguments = (ins FlatSymbolRefAttr:$system, Variadic<AnyType>:$args);
  let assemblyFormat = "$system `(` $args `)` attr-dict (`:` type($args)^)?";
}

def Ecs_YieldOp : Ecs_Op<"yield", [
    Pure, Terminator, ParentOneOf<["SystemOp", "ScheduleOp", "QueryOp"]>]> {
  let summary = "Region terminator; implicit in the custom syntax";
  let assemblyFormat = "attr-dict";
}

#endif // ECS_OPS
