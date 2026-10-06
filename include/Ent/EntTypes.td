#ifndef ENT_TYPES
#define ENT_TYPES

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/BuiltinAttributes.td"
include "Ent/EntDialect.td"

class Ent_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Ent_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def Ent_RefType : Ent_Type<"Ref", "ref"> {
  let summary = "Reference to one component of the entity a query visits";
  let description = [{
    `!ent.ref<@Position>` grants read access, `!ent.ref<@Position, mut>`
    grants read and write access. A ref only exists as a block argument of
    an `ent.query`, so it can never outlive the iteration step.

    `!ent.ref<@Position, up @ChildOf>` is a ref to the component of
    another entity: the nearest ancestor of the visited one along the tree
    relation `@ChildOf` (its parent, the parent's parent, ...) that has the
    component. `ent.get` reads through it; with `mut` (`!ent.ref<@Position,
    mut, up @ChildOf>`) `ent.combine` combines values into its fields.
  }];
  let parameters = (ins "FlatSymbolRefAttr":$component, "bool":$isMutable,
                        "FlatSymbolRefAttr":$via);
  let hasCustomAssemblyFormat = 1;
  let builders = [
    TypeBuilder<(ins "FlatSymbolRefAttr":$component, "bool":$isMutable), [{
      return $_get($_ctxt, component, isMutable, ::mlir::FlatSymbolRefAttr());
    }]>
  ];
  let extraClassDeclaration = [{
    /// Whether the ref is to a component of an ancestor (`up @Relation`).
    bool isUp() const { return static_cast<bool>(getVia()); }
  }];
}

def Ent_EntityType : Ent_Type<"Entity", "entity"> {
  let summary = "The id of an entity";
  let description = [{
    A stable id: it stays valid while the entity moves between rows and
    archetypes and stops being alive when the entity is despawned. A
    component or resource field of this type is a relation. How many bits
    an id takes is decided by the world layout, not by the program.
  }];
}

def Ent_TextType : Ent_Type<"Text", "text"> {
  let summary = "Text or bytes of a fixed capacity, stored inline";
  let description = [{
    `!ent.text<30>` holds up to 30 bytes and how many it holds. It is a
    plain value: stored in the column itself, copied whole, with no
    lifetime. Its storage is one integer of `getStorageBytes()` bytes
    (capacity plus a 16-bit length, rounded up to 16 bytes): the low 16
    bits are the length, byte `i` sits at bits `16 + 8 * i`, and every
    byte past the length is zero, so two texts are equal exactly when
    their integers are. In memory (little-endian) that is the length
    followed by the bytes.
  }];
  let parameters = (ins "unsigned":$capacity);
  let assemblyFormat = "`<` $capacity `>`";
  let extraClassDeclaration = [{
    static constexpr unsigned kMaxCapacity = 4094;
    unsigned getStorageBytes() const {
      return (getCapacity() + 2 + 15) / 16 * 16;
    }
    ::mlir::IntegerType getStorageType() const {
      return ::mlir::IntegerType::get(getContext(), getStorageBytes() * 8);
    }
  }];
}

def Ent_EnumType : Ent_Type<"Enum", "enum"> {
  let summary = "One of the cases an `ent.enum` names";
  let description = [{
    `!ent.enum<@Way>` is a value of the enum `@Way`: one of its cases, and
    a type of its own, so a `@Way` does not go where a `@Mode` belongs. It
    is stored as one byte, the number of the case (the first is 0), and
    computed with as that byte: `builtin.unrealized_conversion_cast` takes
    a value to and from `i8`, as for a text and its integer.
  }];
  let parameters = (ins "FlatSymbolRefAttr":$name);
  let hasCustomAssemblyFormat = 1;
  let extraClassDeclaration = [{
    /// At most this many cases: what a byte tells apart.
    static constexpr unsigned kMaxCases = 256;
    ::mlir::IntegerType getStorageType() const {
      return ::mlir::IntegerType::get(getContext(), 8);
    }
  }];
}

#endif // ENT_TYPES
