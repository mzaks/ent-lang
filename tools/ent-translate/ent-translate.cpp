#include "Ent/EntDialect.h"
#include "Ent/EntOps.h"
#include "Ent/Import.h"
#include "Ent/Structure.h"
#include "Ent/World.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"
#include "mlir/Tools/mlir-translate/Translation.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"

#include <set>

using namespace mlir;
using namespace mlir::ent;

/// The C type that holds one element of `type` in world storage.
static StringRef getCType(Type type) {
  if (isa<EntityType>(type))
    return "ent_entity";
  if (auto named = dyn_cast<EnumType>(type)) {
    // As EnumOp::getCName names it.
    static llvm::StringSet<> names;
    std::string name = "ent_";
    for (char c : named.getName().getValue())
      name += llvm::isAlnum(c) ? c : '_';
    return names.insert(name).first->getKey();
  }
  if (auto text = dyn_cast<TextType>(type)) {
    static llvm::StringSet<> names;
    return names.insert(llvm::formatv("ent_text{0}", text.getCapacity()).str())
        .first->getKey();
  }
  // A text of any length: the address of its block (none: no bytes).
  if (isa<StringType>(type))
    return "const ent_text *";
  if (isa<IndexType>(type))
    return "int64_t";
  if (auto integer = dyn_cast<IntegerType>(type)) {
    switch (integer.getWidth()) {
    case 1:
      return "bool";
    case 8:
      return "int8_t";
    case 16:
      return "int16_t";
    case 32:
      return "int32_t";
    case 64:
      return "int64_t";
    }
  }
  if (type.isF32())
    return "float";
  if (type.isF64())
    return "double";
  return {};
}

/// Replace every character that cannot appear in a C identifier by '_'.
static std::string toIdentifier(StringRef name) {
  std::string result;
  for (char c : name)
    result += llvm::isAlnum(c) ? c : '_';
  if (result.empty() || llvm::isDigit(result.front()))
    result.insert(result.begin(), '_');
  return result;
}

/// What both headers start with after their includes: they are C, which
/// C++ may include too.
static void emitLanguageOpen(raw_ostream &os) {
  os << "\n#ifdef __cplusplus\nextern \"C\" {\n"
        "#define ENT__STATIC_ASSERT static_assert\n#else\n"
        "#define ENT__STATIC_ASSERT _Static_assert\n#endif\n";
}

static void emitLanguageClose(raw_ostream &os) {
  os << "\n#undef ENT__STATIC_ASSERT\n"
        "#ifdef __cplusplus\n} // extern \"C\"\n#endif\n";
}

/// Emit the struct of every text capacity in `capacities`, laid out as
/// the program stores them. Each is guarded, so both generated headers
/// can be included together.
static void emitTexts(ModuleOp module, raw_ostream &os,
                      const std::set<unsigned> &capacities) {
  os << "\n// A text of any length: `length` bytes of `bytes`, and a 0 "
        "after them.\n// (A field holds the address of one, or none for no "
        "bytes; the program\n// keeps them: C reads them.)\n"
        "#ifndef ENT_TEXT\n#define ENT_TEXT\n"
        "typedef struct {\n  uint32_t length;\n"
        "#ifdef __cplusplus\n  char bytes[1];\n#else\n  char bytes[];\n"
        "#endif\n} ent_text;\n#endif\n";
  if (!capacities.empty())
    os << "\n// Texts: `length` bytes of `bytes` count, the rest are zero "
          "(keep them\n// so when writing: equal texts are equal byte for "
          "byte). Not terminated.\n";
  for (unsigned capacity : capacities) {
    unsigned bytes = TextType::get(module.getContext(), capacity)
                         .getStorageBytes();
    std::string padding;
    if (bytes > capacity + 2)
      padding = llvm::formatv("  char ent__padding[{0}];\n",
                              bytes - capacity - 2);
    os << llvm::formatv(
        "#ifndef ENT_TEXT{0}\n#define ENT_TEXT{0}\n"
        "typedef struct {{\n  uint16_t length;\n  char bytes[{0}];\n{1}"
        "} ent_text{0};\n"
        "ENT__STATIC_ASSERT(sizeof(ent_text{0}) == {2}, \"text layout\");\n"
        "#endif\n",
        capacity, padding, bytes);
  }
}

/// The text capacities the extern fns and procs of `module` take.
static void addFunctionTexts(ModuleOp module, std::set<unsigned> &capacities) {
  for (FunctionOp function : module.getOps<FunctionOp>()) {
    if (function.isDefined())
      continue;
    for (Type type : function.getParams().getAsValueRange<TypeAttr>())
      if (auto text = dyn_cast<TextType>(type))
        capacities.insert(text.getCapacity());
  }
}

/// Declare the enums of `module`: a byte, and a name for each case.
static void emitEnums(ModuleOp module, raw_ostream &os) {
  bool any = false;
  for (EnumOp named : module.getOps<EnumOp>()) {
    if (!any)
      os << "\n// Enums: a value is the number of its case, in one byte.\n";
    any = true;
    std::string name = named.getCName();
    // (One of more than a byte tells apart: of as many bits as it says.)
    os << llvm::formatv("#ifndef ENT_ENUM_{0}\n#define ENT_ENUM_{0}\n"
                        "typedef uint{1}_t {0};\nenum {{",
                        name,
                        named->hasAttr("bits")
                            ? named->getAttrOfType<IntegerAttr>("bits").getInt()
                            : 8);
    for (auto [index, label] :
         llvm::enumerate(named.getCases().getAsValueRange<StringAttr>()))
      os << llvm::formatv("{0}\n  {1}_{2} = {3}", index ? "," : "", name,
                          label, index);
    os << "\n};\n#endif\n";
  }
}

/// Declare the C function of every extern fn and proc of `module`; a fn
/// with a body is the program's own, and no business of C.
static LogicalResult
emitFunctions(ModuleOp module, raw_ostream &os,
              function_ref<LogicalResult(Operation *, const std::string &)>
                  claim) {
  bool any = false;
  for (FunctionOp function : module.getOps<FunctionOp>()) {
    if (function.isDefined())
      continue;
    if (failed(claim(function, function.getCName())))
      return failure();
    if (!any)
      os << "\n// Extern fns and procs. The program's systems call these; "
            "define each\n// one. They get values and never the world. A fn "
            "gives the same result\n// for the same arguments and does "
            "nothing else: the program may call it\n// from several threads, "
            "or not at all.\n";
    any = true;
    SmallVector<std::string> params;
    for (auto [index, type] :
         llvm::enumerate(function.getParams().getAsValueRange<TypeAttr>())) {
      if (isa<TextType>(type))
        params.push_back(
            llvm::formatv("const {0} *arg{1}", getCType(type), index));
      else
        params.push_back(llvm::formatv("{0} arg{1}", getCType(type), index));
    }
    std::optional<Type> result = function.getResult();
    os << llvm::formatv("{0} {1}({2});\n",
                        result ? getCType(*result) : StringRef("void"),
                        function.getCName(),
                        params.empty() ? "void" : llvm::join(params, ", "));
  }
  return success();
}

/// Emit what the C of a device needs and nothing more: the extern fns and
/// procs it defines and the texts they take. The world is not in it.
static LogicalResult emitExternHeader(ModuleOp module, raw_ostream &os) {
  llvm::StringSet<> names;
  auto claim = [&](Operation *op, const std::string &name) -> LogicalResult {
    if (!names.insert(name).second)
      return op->emitError("C name '") << name << "' is generated twice";
    return success();
  };
  os << "// Generated by ent-translate --ent-to-c-extern-header. Do not "
        "edit.\n//\n"
        "// The functions a program's systems call that are implemented in "
        "C, for\n// the C that implements them.\n\n"
        "#ifndef ENT_GENERATED_EXTERN_H\n"
        "#define ENT_GENERATED_EXTERN_H\n\n"
        "#include <stdbool.h>\n#include <stdint.h>\n";
  emitLanguageOpen(os);
  std::set<unsigned> textCapacities;
  addFunctionTexts(module, textCapacities);
  emitTexts(module, os, textCapacities);
  emitEnums(module, os);
  if (failed(emitFunctions(module, os, claim)))
    return failure();
  emitLanguageClose(os);
  os << "\n#endif // ENT_GENERATED_EXTERN_H\n";
  return success();
}

/// Emit the C API of the world and the schedules of `module`: creation,
/// counts, typed column accessors and one entry point per schedule. The
/// header is the only place a host needs to know the layout.
static LogicalResult emitHeader(ModuleOp module, raw_ostream &os) {
  // The same archetypes the lowering infers, under the same names.
  if (failed(inferArchetypes(module)))
    return failure();
  FailureOr<WorldLayout> layout = WorldLayout::compute(module);
  if (failed(layout))
    return failure();

  // Distinct names in the program may map to the same C identifier.
  llvm::StringSet<> names;
  auto claim = [&](Operation *op, const std::string &name) -> LogicalResult {
    if (!names.insert(name).second)
      return op->emitError("C name '") << name << "' is generated twice";
    return success();
  };

  os << "// Generated by ent-translate --ent-to-c-header. Do not edit.\n"
        "//\n"
        "// The world is one arena with a layout fixed at compile time. "
        "Create it,\n"
        "// spawn entities (ent_<Archetype>_spawn), fill their columns, and "
        "run\n"
        "// schedules on it.\n\n"
        "#ifndef ENT_GENERATED_WORLD_H\n"
        "#define ENT_GENERATED_WORLD_H\n\n"
        "#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n"
        "#include <string.h>\n";
  emitLanguageOpen(os);
  os << "\ntypedef struct ent_world ent_world;\n\n";
  os << llvm::formatv("#define ENT_WORLD_BYTES {0}ull\n", layout->totalBytes);
  os << llvm::formatv("#define ENT_WORLD_ALIGNMENT {0}\n\n",
                      WorldLayout::kArenaAlignment);

  os << "// Counts and resources start at zero, and so do the slots of "
        "trees, where\n// zero is no edge. Columns are left "
        "uninitialised:\n// nothing reads beyond an archetype's count, and "
        "untouched pages cost\n// no memory.\n"
        "static inline ent_world *ent_world_create(void) {\n"
        "  void *arena = NULL;\n"
        "  if (posix_memalign(&arena, ENT_WORLD_ALIGNMENT, ENT_WORLD_BYTES))\n"
        "    return NULL;\n";
  os << llvm::formatv("  memset(arena, 0, {0});\n", layout->headerBytes);
  // The columns in which zero says "none" (a tree's slots and links).
  for (auto [offset, bytes] : layout->zeroed)
    os << llvm::formatv("  memset((char *)arena + {0}, 0, {1});\n", offset,
                        bytes);
  os << "  return (ent_world *)arena;\n}\n\n";
  // A world is given back with the texts of any length its fields hold:
  // each a block of its own (as a program's `main` gives them back at
  // its end).
  std::string drops;
  for (const WorldArchetype &archetype : layout->archetypes)
    for (const WorldColumn &column : archetype.columns)
      if (isa<StringType>(column.type))
        drops += llvm::formatv(
                     "  for (int64_t row = 0, rows = ((const int64_t *)"
                     "world)[{0}]; row < rows; ++row)\n"
                     "    ent_text_drop(((const uint64_t *)((char *)world + "
                     "{1}))[row]);\n",
                     archetype.index, column.offset)
                     .str();
  for (const WorldResource &resource : layout->resources)
    for (const WorldResourceField &field : resource.fields)
      if (isa<StringType>(field.type))
        drops += llvm::formatv("  ent_text_drop(*(const uint64_t *)((char *)"
                               "world + {0}));\n",
                               field.offset)
                     .str();
  if (!drops.empty())
    os << "void ent_text_drop(uint64_t held);\n";
  os << "static inline void ent_world_destroy(ent_world *world) {\n"
        "  if (!world)\n    return;\n"
     << drops << "  free(world);\n}\n";

  // Texts: one struct per capacity some field has, laid out as the program
  // stores them.
  std::set<unsigned> textCapacities;
  for (Operation &op : module.getOps())
    if (isa<ComponentOp, ResourceOp, RelationOp>(op))
      for (Type type : cast<ArrayAttr>(op.getAttr("field_types"))
                           .getAsValueRange<TypeAttr>())
        if (auto text = dyn_cast<TextType>(type))
          textCapacities.insert(text.getCapacity());
  addFunctionTexts(module, textCapacities);
  // (And those a schedule or an extern system takes.)
  for (ScheduleOp schedule : module.getOps<ScheduleOp>())
    for (Type type : schedule.getBody().getArgumentTypes())
      if (auto text = dyn_cast<TextType>(type))
        textCapacities.insert(text.getCapacity());
  for (ExternOp external : module.getOps<ExternOp>())
    for (Type type : external.getParams().getAsValueRange<TypeAttr>())
      if (auto text = dyn_cast<TextType>(type))
        textCapacities.insert(text.getCapacity());
  emitTexts(module, os, textCapacities);
  emitEnums(module, os);

  // Entity ids, as EntityScheme chose them. The functions below do exactly
  // what the lowered program does.
  const EntityScheme &scheme = layout->entities;
  auto uintType = [](unsigned bits) {
    return llvm::formatv("uint{0}_t", bits).str();
  };
  os << "\n// Entities: ";
  switch (scheme.kind) {
  case EntityScheme::Rows:
    os << "nothing is ever despawned or moved, so an id is\n// "
          "`archetype << ENT__ROW_BITS | row`.\n";
    break;
  case EntityScheme::Slots:
    os << "entities move but are never despawned, so an id is a\n// "
          "slot of the entity table, which records each entity's location "
          "as\n// `archetype << ENT__ROW_BITS | row`.\n";
    break;
  case EntityScheme::Generational:
    os << "an id is `generation << ENT__SLOT_BITS | slot`. Despawning\n// "
          "bumps the slot's generation, so old ids stop being alive; freed "
          "slots\n// are reused last freed first, through a list threaded "
          "through their\n// locations (`archetype << ENT__ROW_BITS | "
          "row`).\n";
    break;
  }
  os << llvm::formatv("typedef {0} ent_entity;\n", uintType(scheme.idBits));
  os << llvm::formatv("#define ENT_NO_ENTITY UINT{0}_MAX\n", scheme.idBits);
  os << llvm::formatv("#define ENT_ENTITY_CAPACITY {0}\n",
                      layout->entityCapacity);
  os << llvm::formatv("#define ENT__ROW_BITS {0}\n", scheme.rowBits);
  os << "static inline int64_t *ent__counts(const ent_world *world) {\n"
        "  return (int64_t *)world;\n}\n";

  std::string allocate, alive, archetypeOf, rowOf;
  if (scheme.kind == EntityScheme::Rows) {
    allocate = "  return ((ent_entity)archetype << ENT__ROW_BITS) | "
               "(ent_entity)row;\n";
    os << llvm::formatv("#define ENT__ARCHETYPES {0}\n",
                        layout->archetypes.size());
    alive = "  uint64_t archetype = (uint64_t)id >> ENT__ROW_BITS;\n"
            "  uint64_t row = (uint64_t)id & ((UINT64_C(1) << ENT__ROW_BITS) "
            "- 1);\n"
            "  return id != ENT_NO_ENTITY && archetype < ENT__ARCHETYPES &&\n"
            "         (int64_t)row < ent__counts(world)[archetype];\n";
    archetypeOf = "(int32_t)((uint64_t)id >> ENT__ROW_BITS)";
    rowOf = "(int64_t)((uint64_t)id & ((UINT64_C(1) << ENT__ROW_BITS) - 1))";
  } else {
    std::string locationType = uintType(scheme.locationBits);
    os << llvm::formatv("#define ENT__SLOT_BITS {0}\n", scheme.slotBits);
    os << llvm::formatv(
        "#define ENT__NEXT_SLOT ((int64_t *)((char *)world + {0}))\n"
        "#define ENT__LOCATION (({1} *)((char *)world + {2}))\n",
        layout->nextSlotOffset, locationType, layout->locationOffset);
    std::string location = llvm::formatv(
        "  ENT__LOCATION[slot] = ({0})(((uint64_t)archetype << ENT__ROW_BITS) "
        "|\n                             (uint64_t)row);\n",
        locationType);
    std::string inUse = "(int64_t)ent__slot(id) < *ENT__NEXT_SLOT";
    if (scheme.kind == EntityScheme::Slots) {
      allocate = "  int64_t slot = (*ENT__NEXT_SLOT)++;\n" + location +
                 "  return (ent_entity)slot;\n";
      alive = "  return id != ENT_NO_ENTITY && " + inUse + ";\n";
    } else {
      os << llvm::formatv(
          "#define ENT__GENERATION_MASK {0}\n"
          "#define ENT__FREE_HEAD ((int64_t *)((char *)world + {1}))\n"
          "#define ENT__GENERATION (({2} *)((char *)world + {3}))\n",
          (uint64_t(1) << scheme.generationBits) - 1, layout->freeHeadOffset,
          uintType(scheme.generationStorageBits), layout->generationOffset);
      allocate = "  int64_t slot;\n"
                 "  if (*ENT__FREE_HEAD != 0) {\n"
                 "    slot = *ENT__FREE_HEAD - 1;\n"
                 "    *ENT__FREE_HEAD = (int64_t)ENT__LOCATION[slot];\n"
                 "  } else {\n"
                 "    slot = (*ENT__NEXT_SLOT)++;\n"
                 "    ENT__GENERATION[slot] = 0;\n"
                 "  }\n" +
                 location +
                 "  return ((ent_entity)ENT__GENERATION[slot] << "
                 "ENT__SLOT_BITS) |\n         (ent_entity)slot;\n";
      alive = "  return id != ENT_NO_ENTITY && " + inUse +
              " &&\n         ENT__GENERATION[ent__slot(id)] ==\n"
              "             (((uint64_t)id >> ENT__SLOT_BITS) & "
              "ENT__GENERATION_MASK);\n";
    }
    os << "static inline uint64_t ent__slot(ent_entity id) {\n"
          "  return (uint64_t)id & ((UINT64_C(1) << ENT__SLOT_BITS) - 1);\n"
          "}\n";
    archetypeOf = "(int32_t)(ENT__LOCATION[ent__slot(id)] >> ENT__ROW_BITS)";
    rowOf = "(int64_t)(ENT__LOCATION[ent__slot(id)] &\n"
            "                   ((UINT64_C(1) << ENT__ROW_BITS) - 1))";
  }
  os << "static inline ent_entity ent__allocate(ent_world *world, "
        "int32_t archetype,\n"
        "                                       int64_t row) {\n";
  if (scheme.kind == EntityScheme::Rows)
    os << "  (void)world;\n";
  os << allocate << "}\n";
  os << "static inline bool ent_entity_alive(const ent_world *world_, "
        "ent_entity id) {\n"
        "  ent_world *world = (ent_world *)world_;\n"
     << alive << "}\n";
  os << "// The archetype (ENT_ARCHETYPE_<name>) and row of a live entity, "
        "or -1.\n"
        "static inline int32_t ent_entity_archetype(const ent_world *world_,\n"
        "                                           ent_entity id) {\n"
        "  ent_world *world = (ent_world *)world_;\n"
        "  if (!ent_entity_alive(world, id))\n    return -1;\n"
        "  return "
     << archetypeOf
     << ";\n}\n"
        "static inline int64_t ent_entity_row(const ent_world *world_, "
        "ent_entity id) {\n"
        "  ent_world *world = (ent_world *)world_;\n"
        "  if (!ent_entity_alive(world, id))\n    return -1;\n"
        "  return "
     << rowOf << ";\n}\n";

  for (const WorldArchetype &archetype : layout->archetypes) {
    ArchetypeOp archetypeOp = archetype.op;
    std::string name = toIdentifier(archetypeOp.getSymName());
    if (failed(claim(archetypeOp, name)))
      return failure();
    os << llvm::formatv("\n// Archetype @{0}\n", archetypeOp.getSymName());
    os << llvm::formatv("#define ENT_{0}_CAPACITY {1}\n", name,
                        archetype.capacity);
    os << llvm::formatv(
        "static inline int64_t ent_{0}_count(const ent_world *world) {{\n"
        "  return ((const int64_t *)world)[{1}];\n}\n",
        name, archetype.index);
    os << llvm::formatv("#define ENT_ARCHETYPE_{0} {1}\n", name,
                        archetype.index);
    // Spawn one entity: the next row, a new id, optional components absent.
    // The same steps as ent.spawn in the lowered program.
    // A new entity starts without its optional components; for reactive
    // queries it has added and changed every other component (stamped with
    // the current tick, one past the counter) and lost none.
    std::string clearPresence, appendToLogs;
    for (const WorldColumn &column : archetype.columns) {
      if (column.isPresence()) {
        clearPresence +=
            llvm::formatv("  ((uint8_t *)((char *)world + {0}))[n] = 0;\n",
                          column.offset);
      } else if (column.isStamp()) {
        bool happened =
            column.stamp->kind != Trigger::Removed &&
            !archetypeOp.isOptional(
                FlatSymbolRefAttr::get(column.stamp->component));
        clearPresence += llvm::formatv(
            "  ((int64_t *)((char *)world + {0}))[n] = {1};\n", column.offset,
            happened ? llvm::formatv("*(int64_t *)((char *)world + {0}) + 1",
                                     layout->tickOffset)
                           .str()
                     : std::string("0"));
        // A new entity is an event: append it to the stamp's log (the
        // segment of its row's low bits), while the segment has room for
        // its slowest reader; one that finds it full is lost to every
        // reader, which the segment's third number tells them (see
        // appendToLog in LowerToLoops.cpp).
        const WorldLog *log = happened ? layout->findLog(*column.stamp)
                                       : nullptr;
        if (log)
          appendToLogs += llvm::formatv(
              "  {{\n"
              "    int64_t segment = n & {0};\n"
              "    int64_t *counts = (int64_t *)((char *)world + {1}) + "
              "segment * {2};\n"
              "    int64_t pending = counts[0] - counts[1];\n"
              "    if (pending < {3}) {{\n"
              "      int64_t slot = segment * {3} + (counts[0]++ & {4});\n"
              "      ((ent_entity *)((char *)world + {5}))[slot] = id;\n"
              "      ((int64_t *)((char *)world + {6}))[slot] =\n"
              "          *(int64_t *)((char *)world + {7}) + 1;\n"
              "    } else if (!counts[3]) {{ // overflowed: every reader "
              "scans\n"
              "      counts[0] = (pending > {3} ? counts[0] : counts[1] + {3}) "
              "+ 1;\n"
              "      counts[2] = counts[0];\n"
              "      counts[3] = 1;\n"
              "    }\n  }\n",
              log->segments - 1, log->countsOffset,
              WorldLog::kSegmentStride / 8, log->segmentCapacity,
              log->segmentCapacity - 1, log->idsOffset, log->ticksOffset,
              layout->tickOffset);
      }
    }
    os << "// Returns the new entity's id, or ENT_NO_ENTITY if the archetype "
          "is full.\n// Its fields are uninitialised; fill them at "
          "ent_entity_row(world, id).\n";
    std::string storeId =
        scheme.hasIds()
            ? llvm::formatv("  ((ent_entity *)((char *)world + {0}))[n] = "
                            "id;\n",
                            archetype.idOffset)
                  .str()
            : std::string();
    // A new entity is out of a sorted archetype's order: its tree is
    // sorted again when a schedule next starts.
    if (archetype.isSorted())
      storeId += llvm::formatv(
                     "  *(int64_t *)((char *)world + {0}) = 0; // unsorted\n",
                     layout->getRelation(archetype.sortedBy).cleanOffset)
                     .str();
    os << llvm::formatv(
        "static inline ent_entity ent_{0}_spawn(ent_world *world) {{\n"
        "  int64_t n = ((int64_t *)world)[{1}];\n"
        "  if (n >= ENT_{0}_CAPACITY)\n    return ENT_NO_ENTITY;\n"
        "{2}"
        "  ent_entity id = ent__allocate(world, {1}, n);\n"
        "{3}{4}"
        "  ((int64_t *)world)[{1}] = n + 1;\n"
        "  return id;\n}\n",
        name, archetype.index, clearPresence, storeId, appendToLogs);
    os << llvm::formatv(
        "// Spawns n entities in consecutive rows; false (and none spawned) "
        "if they\n// do not fit.\n"
        "static inline bool ent_{0}_spawn_n(ent_world *world, int64_t n) {{\n"
        "  if (n < 0 || ent_{0}_count(world) + n > ENT_{0}_CAPACITY)\n"
        "    return false;\n"
        "  for (int64_t i = 0; i < n; ++i)\n"
        "    ent_{0}_spawn(world);\n"
        "  return true;\n}\n",
        name);
    if (scheme.hasIds()) {
      os << llvm::formatv(
          "// The id of the entity in each row.\n"
          "static inline ent_entity *ent_{0}_id(ent_world *world) {{\n"
          "  return (ent_entity *)((char *)world + {1});\n}\n",
          name, archetype.idOffset);
      if (failed(claim(archetypeOp, "ent_" + name + "_id")))
        return failure();
    }
    for (const WorldColumn &column : archetype.columns) {
      // Stamps are the compiler's bookkeeping for reactive queries.
      if (column.isStamp())
        continue;
      bool isPresence = column.isPresence();
      std::string accessor = llvm::formatv(
          "ent_{0}_{1}_{2}", name, toIdentifier(column.component.getValue()),
          isPresence ? std::string("present")
                     : toIdentifier(column.field.getValue()));
      if (failed(claim(archetypeOp, accessor)))
        return failure();
      StringRef cType = isPresence ? "uint8_t" : getCType(column.type);
      os << llvm::formatv(
          "static inline {0} *{1}(ent_world *world) {{\n"
          "  return ({0} *)((char *)world + {2});\n}\n",
          cType, accessor, column.offset);
    }
  }

  for (const WorldResource &resource : layout->resources) {
    ResourceOp resourceOp = resource.op;
    std::string name = toIdentifier(resourceOp.getSymName());
    os << llvm::formatv("\n// Resource @{0}\n", resourceOp.getSymName());
    for (const WorldResourceField &field : resource.fields) {
      std::string accessor = llvm::formatv(
          "ent_{0}_{1}", name, toIdentifier(field.field.getValue()));
      if (failed(claim(resourceOp, accessor)))
        return failure();
      os << llvm::formatv(
          "static inline {0} *{1}(ent_world *world) {{\n"
          "  return ({0} *)((char *)world + {2});\n}\n",
          getCType(field.type), accessor, field.offset);
    }
  }

  for (const WorldRelation &relation : layout->relations) {
    RelationOp relationOp = relation.op;
    std::string name = toIdentifier(relationOp.getSymName());
    if (relation.linked)
      os << llvm::formatv(
          "\n// Relation @{0}, a tree: every entity has at most one edge, "
          "to its\n// parent, which ent_{1}_parent gives. The fields' "
          "columns have a slot per\n// entity, at ent_{1}_slot(id). "
          "ent_{1}_connect sets an entity's edge; the\n// next schedule run "
          "takes the edges in, dropping those of and to\n// entities no "
          "longer alive.\n",
          relationOp.getSymName(), name);
    else
      os << llvm::formatv(
        "\n// Relation @{0}: edges from a source to a target entity. "
        "ent_{1}_connect\n// appends one; the next schedule run sorts them "
        "by {2} (stable: the\n// edges of one {3} keep their order), "
        "dropping edges to entities no\n// longer alive. The columns below "
        "hold the first ent_{1}_count(world)\n// edges, in that order once "
        "sorted.\n",
        relationOp.getSymName(), name,
        relation.byTarget ? "target, then source" : "source",
        relation.byTarget ? "target and source" : "source");
    os << llvm::formatv("#define ENT_{0}_CAPACITY {1}\n", name,
                        relation.capacity);
    os << llvm::formatv(
        "static inline int64_t *ent__{0}_count(ent_world *world) {{\n"
        "  return (int64_t *)((char *)world + {1});\n}\n"
        "static inline int64_t ent_{0}_count(ent_world *world) {{\n"
        "  return *ent__{0}_count(world);\n}\n",
        name, relation.countOffset);
    if (relation.linked)
      // A slot's owner is its edge's source and one: 0 is no edge.
      os << llvm::formatv(
          "static inline ent_entity *ent__{0}_owner(ent_world *world) {{\n"
          "  return (ent_entity *)((char *)world + {1});\n}\n"
          "static inline ent_entity *ent__{0}_target(ent_world *world) {{\n"
          "  return (ent_entity *)((char *)world + {2});\n}\n"
          "static inline uint64_t ent_{0}_slot(ent_entity id) {{\n"
          "  return {3};\n}\n"
          "// The entity `id` has its edge to, or ENT_NO_ENTITY.\n"
          "static inline ent_entity ent_{0}_parent(ent_world *world, "
          "ent_entity id) {{\n"
          "  uint64_t at = ent_{0}_slot(id);\n"
          "  return ent__{0}_owner(world)[at] == (ent_entity)(id + 1)\n"
          "             ? ent__{0}_target(world)[at]\n"
          "             : ENT_NO_ENTITY;\n}\n",
          name, relation.sourceOffset, relation.targetOffset,
          scheme.hasIds()
              ? "(uint64_t)id & ((UINT64_C(1) << ENT__SLOT_BITS) - 1)"
              : "(uint64_t)id");
    else
    os << llvm::formatv(
        "static inline ent_entity *ent_{0}_source(ent_world *world) {{\n"
        "  return (ent_entity *)((char *)world + {1});\n}\n"
        "static inline ent_entity *ent_{0}_target(ent_world *world) {{\n"
        "  return (ent_entity *)((char *)world + {2});\n}\n",
        name, relation.sourceOffset, relation.targetOffset);
    std::string params, stores;
    for (const WorldColumn &field : relation.fields) {
      std::string fieldName = toIdentifier(field.field.getValue());
      std::string accessor = llvm::formatv("ent_{0}_{1}", name, fieldName);
      if (failed(claim(relationOp, accessor)))
        return failure();
      os << llvm::formatv(
          "static inline {0} *{1}(ent_world *world) {{\n"
          "  return ({0} *)((char *)world + {2});\n}\n",
          getCType(field.type), accessor, field.offset);
      params += llvm::formatv(", {0} value_{1}", getCType(field.type),
                              fieldName)
                    .str();
      stores += llvm::formatv("  {0}(world)[{2}] = value_{1};\n", accessor,
                              fieldName, relation.linked ? "at" : "*count")
                    .str();
    }
    if (relation.deadOffset)
      stores += llvm::formatv("  ((uint8_t *)((char *)world + {0}))[{1}] "
                              "= 0;\n",
                              relation.deadOffset,
                              relation.linked ? "at" : "*count")
                    .str();
    // The components the ends must have, checked like the generated code
    // checks an in-language connect.
    std::string checks;
    for (bool target : {false, true}) {
      FlatSymbolRefAttr component = relationOp.getEndpoint(target);
      if (!component)
        continue;
      std::string function = llvm::formatv(
          "ent__{0}_{1}_has", name, target ? "target" : "source");
      os << llvm::formatv(
          "static inline bool {0}(ent_world *world, ent_entity id) {{\n"
          "  int64_t row = ent_entity_row(world, id);\n"
          "  switch (ent_entity_archetype(world, id)) {{\n",
          function);
      for (const WorldArchetype &archetype : layout->archetypes) {
        ArchetypeOp archetypeOp = archetype.op;
        if (!archetypeOp.contains(component))
          continue;
        std::string archetypeName = toIdentifier(archetypeOp.getSymName());
        os << llvm::formatv("  case ENT_ARCHETYPE_{0}:\n", archetypeName);
        if (archetypeOp.isOptional(component))
          os << llvm::formatv("    return ent_{0}_{1}_present(world)[row] != "
                              "0;\n",
                              archetypeName,
                              toIdentifier(component.getValue()));
        else
          os << "    return true;\n";
      }
      os << "  default:\n    (void)row;\n    return false;\n  }\n}\n";
      checks += llvm::formatv("  if (!{0}(world, {1}))\n    return false;\n",
                              function, target ? "target" : "source")
                    .str();
    }
    std::string connect = llvm::formatv("ent_{0}_connect", name);
    if (failed(claim(relationOp, connect)))
      return failure();
    // Being connected is an event of the source, where a reactive query
    // has a trigger up the tree (see appendEdge in LowerToLoops.cpp): the
    // tick per entity key, and an entry in the event's log.
    std::string connected;
    if (relation.connectedOffset) {
      connected = llvm::formatv(
          "  {{\n"
          "    uint64_t key = {0};\n"
          "    int64_t *ticks = (int64_t *)((char *)world + {1});\n"
          "    int64_t now = *(int64_t *)((char *)world + {2}) + 1;\n"
          "    ticks[{3}] = now; // the latest connect of any entity\n"
          "    if (ticks[key] != now) {{\n"
          "      ticks[key] = now;\n",
          scheme.hasIds()
              ? "(uint64_t)source & ((UINT64_C(1) << ENT__SLOT_BITS) - 1)"
              : "(uint64_t)source",
          relation.connectedOffset, layout->tickOffset, layout->entityKeys);
      Stamp stamp{Trigger::Connected, relationOp.getSymNameAttr(),
                  StringAttr::get(module.getContext(), "")};
      if (const WorldLog *log = layout->findLog(stamp))
        connected += llvm::formatv(
            "      int64_t segment = key & {0};\n"
            "      int64_t *counts = (int64_t *)((char *)world + {1}) + "
            "segment * {2};\n"
            "      int64_t pending = counts[0] - counts[1];\n"
            "      if (pending < {3}) {{\n"
            "        int64_t slot = segment * {3} + (counts[0]++ & {4});\n"
            "        ((ent_entity *)((char *)world + {5}))[slot] = source;\n"
            "        ((int64_t *)((char *)world + {6}))[slot] = now;\n"
            "      } else if (!counts[3]) {{ // overflowed: every reader "
            "scans\n"
            "        counts[0] = (pending > {3} ? counts[0] : counts[1] + {3}) "
            "+ 1;\n"
            "        counts[2] = counts[0];\n"
            "        counts[3] = 1;\n"
            "      }\n",
            log->segments - 1, log->countsOffset,
            WorldLog::kSegmentStride / 8, log->segmentCapacity,
            log->segmentCapacity - 1, log->idsOffset, log->ticksOffset);
      connected += "    }\n  }\n";
    }
    if (relation.linked)
      // The slot of the source's key; an edge it already has is replaced.
      os << llvm::formatv(
          "// Returns false, connecting nothing, if the relation is full or "
          "an end\n// lacks the component the relation names for it. An "
          "entity that has an\n// edge gets the new one instead.\n"
          "static inline bool {0}(ent_world *world, ent_entity source,\n"
          "                       ent_entity target{1}) {{\n"
          "{5}"
          "  int64_t *count = ent__{2}_count(world);\n"
          "  uint64_t at = ent_{2}_slot(source);\n"
          "  if (ent__{2}_owner(world)[at] != (ent_entity)(source + 1)) {{\n"
          "    if (*count >= ENT_{2}_CAPACITY)\n      return false;\n"
          "    ++*count;\n  }\n"
          "  ent__{2}_owner(world)[at] = (ent_entity)(source + 1);\n"
          "  ent__{2}_target(world)[at] = target;\n"
          "{3}{6}{7}"
          "  *(int64_t *)((char *)world + {4}) = 0; // unclean\n"
          "  return true;\n}\n",
          connect, params, name, stores, relation.cleanOffset, checks,
          connected,
          // The number of the connect, for a tree with an order.
          relation.isOrdered()
              ? llvm::formatv("  {{\n"
                              "    int64_t *numbers = (int64_t *)((char *)"
                              "world + {0});\n"
                              "    numbers[at] = ++numbers[{1}];\n  }\n",
                              relation.sequenceOffset, layout->entityKeys)
                    .str()
              : std::string());
    else
    os << llvm::formatv(
        "// Returns false, connecting nothing, if the relation is full or an "
        "end\n// lacks the component the relation names for it.\n"
        "static inline bool {0}(ent_world *world, ent_entity source,\n"
        "                       ent_entity target{1}) {{\n"
        "{5}"
        "  int64_t *count = ent__{2}_count(world);\n"
        "  if (*count >= ENT_{2}_CAPACITY)\n    return false;\n"
        "  ent_{2}_source(world)[*count] = source;\n"
        "  ent_{2}_target(world)[*count] = target;\n"
        "{3}{6}{7}"
        "  ++*count;\n"
        "  *(int64_t *)((char *)world + {4}) = 0; // unclean\n"
        "  return true;\n}\n",
        connect, params, name, stores, relation.cleanOffset, checks,
        connected,
        relation.isOrdered()
            ? llvm::formatv(
                  "  {{\n"
                  "    int64_t *numbers = (int64_t *)((char *)world + {0});\n"
                  "    numbers[{2}] = ++numbers[{1}];\n  }\n",
                  relation.sequenceOffset, layout->entityKeys,
                  scheme.hasIds() ? "(uint64_t)source & ((UINT64_C(1) << "
                                    "ENT__SLOT_BITS) - 1)"
                                  : "(uint64_t)source")
                  .str()
            : std::string());
  }

  os << "\n// Schedules. The lowered function receives the arena as a "
        "memref descriptor.\n"
        "typedef struct {\n  char *allocated;\n  char *aligned;\n"
        "  int64_t offset;\n  int64_t size;\n  int64_t stride;\n"
        "} ent_arena_descriptor;\n"
        "#ifdef __APPLE__\n#define ENT__SYMBOL(name) __asm__(\"_\" name)\n"
        "#else\n#define ENT__SYMBOL(name) __asm__(name)\n#endif\n";
  for (ScheduleOp schedule : module.getOps<ScheduleOp>()) {
    std::string name = toIdentifier(schedule.getSymName());
    if (failed(claim(schedule, name)))
      return failure();
    SmallVector<std::string> params, args;
    // (A text of a capacity is passed by a pointer to it, through the way
    // in the program has for that.)
    bool byPointer = false;
    for (BlockArgument arg : schedule.getBody().getArguments()) {
      StringRef cType = getCType(arg.getType());
      if (isa<TextType>(arg.getType())) {
        byPointer = true;
        params.push_back(llvm::formatv("const {0} *arg{1}", cType,
                                       arg.getArgNumber()));
        args.push_back(llvm::formatv("arg{0}", arg.getArgNumber()));
        continue;
      }
      if (cType.empty())
        return schedule.emitError("parameter #")
               << arg.getArgNumber() << " has type " << arg.getType()
               << ", which has no C equivalent here";
      params.push_back(llvm::formatv("{0} arg{1}", cType, arg.getArgNumber()));
      args.push_back(llvm::formatv("arg{0}", arg.getArgNumber()));
    }
    std::string declParams, callArgs;
    for (auto [param, arg] : llvm::zip(params, args)) {
      declParams += param + ", ";
      callArgs += arg + ", ";
    }
    // The lowered function is named after the schedule; where that is no
    // C name (a module's schedule, `m.frame`), it is declared under one
    // and bound to the symbol.
    std::string label;
    if (name != schedule.getSymName() || byPointer)
      label = llvm::formatv(" ENT__SYMBOL(\"_mlir_ciface_{0}{1}\")",
                            schedule.getSymName(), byPointer ? ".host" : "");
    os << llvm::formatv("\nvoid _mlir_ciface_{0}({1}ent_arena_descriptor "
                        "*world){2};\n",
                        name, declParams, label);
    os << llvm::formatv(
        "static inline void ent_{0}(ent_world *world{1}{2}) {{\n"
        "  ent_arena_descriptor arena = {{(char *)world, (char *)world, 0,\n"
        "                                 ENT_WORLD_BYTES, 1};\n"
        "  _mlir_ciface_{0}({3}&arena);\n}\n",
        name, params.empty() ? "" : ", ", llvm::join(params, ", "),
        callArgs);
  }

  // Extern systems: C functions the lowered schedules call. Their names
  // share the header's `ent_` prefix with the accessors above.
  bool anyExtern = false;
  for (ExternOp external : module.getOps<ExternOp>()) {
    if (failed(claim(external, external.getCName())))
      return failure();
    if (!anyExtern)
      os << "\n// Extern systems. The program calls these when a schedule "
            "runs them;\n// define each one, working on the world through "
            "this header.\n";
    anyExtern = true;
    std::string params;
    for (auto [index, type] :
         llvm::enumerate(external.getParams().getAsValueRange<TypeAttr>())) {
      StringRef cType = getCType(type);
      if (cType.empty())
        return external.emitError("parameter #")
               << index << " has type " << type
               << ", which has no C equivalent here";
      params += llvm::formatv(isa<TextType>(type) ? ", const {0} *arg{1}"
                                                  : ", {0} arg{1}",
                              cType, index)
                    .str();
    }
    os << llvm::formatv("void {0}(ent_world *world{1});\n",
                        external.getCName(), params);
  }

  if (failed(emitFunctions(module, os, claim)))
    return failure();
  emitLanguageClose(os);

  os << "\n#endif // ENT_GENERATED_WORLD_H\n";
  return success();
}

int main(int argc, char **argv) {
  TranslateToMLIRRegistration import(
      "import-ent", "Parse ent-lang source (a .ent file) into the ent dialect",
      [](llvm::SourceMgr &sourceMgr,
         MLIRContext *context) -> OwningOpRef<Operation *> {
        return importEnt(sourceMgr, context);
      },
      [](DialectRegistry &registry) {
        registry.insert<EntDialect, arith::ArithDialect, scf::SCFDialect>();
      });
  // What a program is made of, for what builds it: one line for each
  // source (`source`, a tab, its path) and each file it declares
  // (`asset`, its name, its path).
  TranslateRegistration sources(
      "ent-to-sources",
      "List the sources of an ent-lang program and the files it declares",
      [](const std::shared_ptr<llvm::SourceMgr> &sourceMgr, raw_ostream &os,
         MLIRContext *context) -> LogicalResult {
        ImportedFiles files;
        if (!importEnt(*sourceMgr, context, {}, &files))
          return failure();
        for (const std::string &path : files.sources)
          os << "source\t" << path << "\n";
        for (auto &[name, path] : files.assets)
          os << "asset\t" << name << "\t" << path << "\n";
        return success();
      });
  TranslateFromMLIRRegistration header(
      "ent-to-c-header", "Emit the C API of an ent-lang program's world",
      [](Operation *op, raw_ostream &os) -> LogicalResult {
        auto module = dyn_cast<ModuleOp>(op);
        if (!module)
          return op->emitError("expected a module");
        return emitHeader(module, os);
      },
      [](DialectRegistry &registry) {
        registry.insert<EntDialect, arith::ArithDialect, func::FuncDialect,
                        scf::SCFDialect>();
      });
  TranslateFromMLIRRegistration externHeader(
      "ent-to-c-extern-header",
      "Emit the C declarations of an ent-lang program's extern fns and procs",
      [](Operation *op, raw_ostream &os) -> LogicalResult {
        auto module = dyn_cast<ModuleOp>(op);
        if (!module)
          return op->emitError("expected a module");
        return emitExternHeader(module, os);
      },
      [](DialectRegistry &registry) {
        registry.insert<EntDialect, arith::ArithDialect, func::FuncDialect,
                        scf::SCFDialect>();
      });
  return failed(mlirTranslateMain(argc, argv, "ent-lang translation tool"));
}
