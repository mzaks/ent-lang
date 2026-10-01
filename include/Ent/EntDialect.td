#ifndef ENT_DIALECT
#define ENT_DIALECT

include "mlir/IR/OpBase.td"

def Ent_Dialect : Dialect {
  let name = "ent";
  let summary = "Entity Component System semantics";
  let description = [{
    Components are plain records with an abstract layout. Systems declare
    which components they read and write; queries inside a system bind one
    reference per component and may only touch what the system declared.
    Schedules name the systems that run each frame. Layout, iteration order
    and parallelism are decided by later passes, not by the program.
  }];
  let cppNamespace = "::mlir::ent";
  let useDefaultTypePrinterParser = 1;
  let extraClassDeclaration = [{
    void registerTypes();
  }];
}

class Ent_Op<string mnemonic, list<Trait> traits = []> :
    Op<Ent_Dialect, mnemonic, traits>;

#endif // ENT_DIALECT
