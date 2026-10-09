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

    `!ent.ref<@Position, parent @ChildOf>` is a ref to the component of
    the visited entity's parent itself, and to nothing if the parent does
    not have the component: no further ancestor is looked at.

    With `hops N` (`!ent.ref<@Position, parent @ChildOf, hops 2>`) it is
    that many steps up: the parent's parent, and so on, whatever those on
    the way have.

    With a path
    (`!ent.ref<@Position, path [["parent", @ChildOf], ["up", @Owns, @Tint]]>`)
    the entity is reached step by step from the visited one, each step
    along a tree of its own: `"parent"` to the parent, `"up"` to the
    nearest ancestor that has all of the components the step names. The
    ref is to the component of the entity the last step leads to, and to
    nothing if a step leads nowhere or that entity does not have it. It
    is read only.

    `!ent.ref<@Position, after @ChildOf>` is the sibling after, as
    `before` is the one before. A query never reads through it a field it
    writes: that sibling is yet to be visited.

    `!ent.ref<@Position, before @ChildOf>` is a ref to the component of the
    visited entity's sibling before it: the child of the same parent that
    comes right before it in the tree's order (`tree ordered by`). It is
    read only.

    With `optional` (`!ent.ref<@Position, up @ChildOf, optional>`) the
    query also visits the entities that have no such ancestor or sibling:
    `ent.bound` says whether the ref leads anywhere, and `ent.get` reads
    zero where it does not.
  }];
  let parameters = (ins "FlatSymbolRefAttr":$component, "bool":$isMutable,
                        "FlatSymbolRefAttr":$via, "bool":$isBefore,
                        "bool":$isOptional, "bool":$isDirect,
                        "bool":$isAfter, "unsigned":$hops,
                        "ArrayAttr":$path);
  let hasCustomAssemblyFormat = 1;
  let builders = [
    TypeBuilder<(ins "FlatSymbolRefAttr":$component, "bool":$isMutable), [{
      return $_get($_ctxt, component, isMutable, ::mlir::FlatSymbolRefAttr(),
                   false, false, false, false, 1u, ::mlir::ArrayAttr());
    }]>,
    TypeBuilder<(ins "FlatSymbolRefAttr":$component, "bool":$isMutable,
                     "FlatSymbolRefAttr":$via), [{
      return $_get($_ctxt, component, isMutable, via, false, false, false,
                   false, 1u, ::mlir::ArrayAttr());
    }]>,
    TypeBuilder<(ins "FlatSymbolRefAttr":$component, "bool":$isMutable,
                     "FlatSymbolRefAttr":$via, "bool":$isBefore,
                     "bool":$isOptional), [{
      return $_get($_ctxt, component, isMutable, via, isBefore, isOptional,
                   false, false, 1u, ::mlir::ArrayAttr());
    }]>
  ];
  let extraClassDeclaration = [{
    /// Whether the ref is to a component of another entity, found along a
    /// tree: an ancestor (`up @Relation`) or, with `isBefore`, the
    /// sibling before (`before @Relation`).
    bool isUp() const { return static_cast<bool>(getVia()); }
    /// Whether the entity is reached by a path of steps (`path [...]`):
    /// `getVia` is then the tree of its first step.
    bool hasPath() const { return getPath() && !getPath().empty(); }
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

def Ent_StringType : Ent_Type<"String", "string"> {
  let summary = "Text of any length, held by the world";
  let description = [{
    `!ent.string` is a text without a capacity (`text` in a program). A
    value of it is a view: the address of a block of a 32-bit length and
    the bytes after it (0: no bytes), which stays where it is as long as
    the value is used. A field of it holds a block of its own: what is
    assigned to the field is copied (`ent.text.own`), and what the field
    held is given back (`ent.text.drop`). It is stored, and computed with,
    as that address: `builtin.unrealized_conversion_cast` takes a value to
    and from `i64`.
  }];
  let extraClassDeclaration = [{
    ::mlir::IntegerType getStorageType() const {
      return ::mlir::IntegerType::get(getContext(), 64);
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

    `!ent.enum<@Rows, 32>` is one of more things than a byte tells apart,
    which have no names: stored and computed with as an integer of that
    many bits (a row of one of the tables of a shape is such a value).
  }];
  let parameters = (ins "FlatSymbolRefAttr":$name, "unsigned":$bits);
  let builders = [
    TypeBuilder<(ins "::mlir::FlatSymbolRefAttr":$name), [{
      return $_get($_ctxt, name, 8u);
    }]>
  ];
  let hasCustomAssemblyFormat = 1;
  let extraClassDeclaration = [{
    /// At most this many cases: what a byte tells apart.
    static constexpr unsigned kMaxCases = 256;
    ::mlir::IntegerType getStorageType() const {
      return ::mlir::IntegerType::get(getContext(), getBits());
    }
  }];
}

#endif // ENT_TYPES
