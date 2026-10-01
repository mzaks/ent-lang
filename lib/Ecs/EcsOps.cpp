#include "Ecs/EcsOps.h"
#include "Ecs/Structure.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace mlir;
using namespace mlir::ecs;

// `(@A, optional @B)`, used by ecs.archetype.
static ParseResult parseComponentList(OpAsmParser &parser, ArrayAttr &list,
                                      ArrayAttr &optional) {
  SmallVector<Attribute> components, optionals;
  auto parseOne = [&]() -> ParseResult {
    bool isOptional = succeeded(parser.parseOptionalKeyword("optional"));
    FlatSymbolRefAttr symbol;
    if (parser.parseAttribute(symbol))
      return failure();
    components.push_back(symbol);
    if (isOptional)
      optionals.push_back(symbol);
    return success();
  };
  if (parser.parseCommaSeparatedList(OpAsmParser::Delimiter::Paren, parseOne))
    return failure();
  list = parser.getBuilder().getArrayAttr(components);
  if (!optionals.empty())
    optional = parser.getBuilder().getArrayAttr(optionals);
  return success();
}

static void printComponentList(OpAsmPrinter &p, Operation *, ArrayAttr list,
                               ArrayAttr optional) {
  p << "(";
  llvm::interleaveComma(list, p, [&](Attribute component) {
    if (optional && llvm::is_contained(optional, component))
      p << "optional ";
    p << component;
  });
  p << ")";
}

// A bare keyword naming how ecs.apply combines values: `add`, `min`, `max`.
static ParseResult parseRule(OpAsmParser &parser, StringAttr &rule) {
  StringRef keyword;
  if (parser.parseKeyword(&keyword))
    return failure();
  rule = parser.getBuilder().getStringAttr(keyword);
  return success();
}

static void printRule(OpAsmPrinter &p, Operation *, StringAttr rule) {
  p << rule.getValue();
}

#define GET_OP_CLASSES
#include "Ecs/EcsOps.cpp.inc"

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

/// Parse `(%a: T, %b: U) attr-dict-with-keyword? {region}` where the
/// arguments become the region's entry block arguments. `beforeRegion`
/// parses anything that sits between the argument list and the region.
template <typename OpTy>
static ParseResult
parseArgsAndBody(OpAsmParser &parser, OperationState &result,
                 function_ref<ParseResult()> beforeRegion = nullptr) {
  SmallVector<OpAsmParser::Argument> args;
  if (parser.parseArgumentList(args, OpAsmParser::Delimiter::Paren,
                               /*allowType=*/true))
    return failure();
  if (beforeRegion && beforeRegion())
    return failure();
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, args))
    return failure();
  OpTy::ensureTerminator(*body, parser.getBuilder(), result.location);
  return success();
}

static void printArgs(OpAsmPrinter &p, Region &body) {
  p << "(";
  llvm::interleaveComma(body.getArguments(), p,
                        [&](BlockArgument arg) { p.printRegionArgument(arg); });
  p << ")";
}

static void printBody(OpAsmPrinter &p, Region &body) {
  p << " ";
  p.printRegion(body, /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
}

/// Refs may only be introduced by `ecs.query`; a system or schedule taking a
/// ref parameter would smuggle component access past the declared sets.
static LogicalResult verifyNoRefParams(Operation *op, Region &body) {
  for (BlockArgument arg : body.getArguments())
    if (isa<RefType>(arg.getType()))
      return op->emitOpError("parameter #")
             << arg.getArgNumber()
             << " is a component reference; references can only be bound "
                "by 'ecs.query'";
  return success();
}

static ComponentOp lookupComponent(SymbolTableCollection &symbolTable,
                                   Operation *from, FlatSymbolRefAttr ref) {
  return symbolTable.lookupNearestSymbolFrom<ComponentOp>(from, ref);
}

/// Resolve the field that a get/set names and return its declared type.
static FailureOr<Type> resolveField(SymbolTableCollection &symbolTable,
                                    Operation *op, RefType refType,
                                    StringRef field) {
  ComponentOp component =
      lookupComponent(symbolTable, op, refType.getComponent());
  if (!component)
    return op->emitOpError("references unknown component ")
           << refType.getComponent();
  Type fieldType = component.getFieldType(field);
  if (!fieldType)
    return op->emitOpError("component ")
           << refType.getComponent() << " has no field '" << field << "'";
  return fieldType;
}

//===----------------------------------------------------------------------===//
// ComponentOp and ResourceOp
//===----------------------------------------------------------------------===//

// @Name (field: type, ...)
template <typename OpTy>
static ParseResult parseRecord(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  SmallVector<Attribute> names, types;
  auto parseField = [&]() -> ParseResult {
    std::string field;
    Type type;
    if (parser.parseKeywordOrString(&field) || parser.parseColon() ||
        parser.parseType(type))
      return failure();
    names.push_back(parser.getBuilder().getStringAttr(field));
    types.push_back(TypeAttr::get(type));
    return success();
  };
  if (parser.parseCommaSeparatedList(OpAsmParser::Delimiter::Paren,
                                     parseField))
    return failure();

  Builder &b = parser.getBuilder();
  result.addAttribute(OpTy::getFieldNamesAttrName(result.name),
                      b.getArrayAttr(names));
  result.addAttribute(OpTy::getFieldTypesAttrName(result.name),
                      b.getArrayAttr(types));
  return parser.parseOptionalAttrDict(result.attributes);
}

template <typename OpTy>
static void printRecord(OpTy op, OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(op.getSymName());
  p << " (";
  llvm::interleaveComma(
      llvm::zip(op.getFieldNames(), op.getFieldTypes()), p, [&](auto field) {
        p.printKeywordOrString(cast<StringAttr>(std::get<0>(field)).getValue());
        p << ": " << cast<TypeAttr>(std::get<1>(field)).getValue();
      });
  p << ")";
  p.printOptionalAttrDict(op->getAttrs(),
                          {op.getSymNameAttrName(), op.getFieldNamesAttrName(),
                           op.getFieldTypesAttrName()});
}

static LogicalResult verifyRecord(Operation *op, ArrayAttr names,
                                  ArrayAttr types) {
  if (names.size() != types.size())
    return op->emitOpError("has ") << names.size() << " field names but "
                                   << types.size() << " field types";

  llvm::StringSet<> seen;
  for (auto [nameAttr, typeAttr] : llvm::zip(names, types)) {
    StringRef name = cast<StringAttr>(nameAttr).getValue();
    // The empty name stands for an optional component's presence column.
    if (name.empty())
      return op->emitOpError("has a field without a name");
    if (!seen.insert(name).second)
      return op->emitOpError("has duplicate field '") << name << "'";
    // Scalars only for now: layout passes split components into one column
    // per field, which needs every field to be a plain value. An entity id
    // is one too (a relation).
    Type type = cast<TypeAttr>(typeAttr).getValue();
    if (!isa<IntegerType, FloatType, IndexType, EntityType>(type))
      return op->emitOpError("field '")
             << name << "' has type " << type
             << "; only integer, float, index and entity fields are "
                "supported";
  }
  return success();
}

static Type lookupFieldType(ArrayAttr names, ArrayAttr types,
                            StringRef name) {
  for (auto [nameAttr, typeAttr] : llvm::zip(names, types))
    if (cast<StringAttr>(nameAttr).getValue() == name)
      return cast<TypeAttr>(typeAttr).getValue();
  return {};
}

ParseResult ComponentOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseRecord<ComponentOp>(parser, result);
}
void ComponentOp::print(OpAsmPrinter &p) { printRecord(*this, p); }
LogicalResult ComponentOp::verify() {
  return verifyRecord(*this, getFieldNames(), getFieldTypes());
}
Type ComponentOp::getFieldType(StringRef name) {
  return lookupFieldType(getFieldNames(), getFieldTypes(), name);
}

ParseResult ResourceOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseRecord<ResourceOp>(parser, result);
}
void ResourceOp::print(OpAsmPrinter &p) { printRecord(*this, p); }
LogicalResult ResourceOp::verify() {
  return verifyRecord(*this, getFieldNames(), getFieldTypes());
}
Type ResourceOp::getFieldType(StringRef name) {
  return lookupFieldType(getFieldNames(), getFieldTypes(), name);
}

//===----------------------------------------------------------------------===//
// ArchetypeOp
//===----------------------------------------------------------------------===//

LogicalResult ArchetypeOp::verify() {
  if (getComponents().empty())
    return emitOpError("must contain at least one component");
  llvm::SmallPtrSet<Attribute, 8> seen;
  for (Attribute attr : getComponents())
    if (!seen.insert(attr).second)
      return emitOpError("lists component ") << attr << " more than once";
  if (ArrayAttr optional = getOptionalAttr())
    for (Attribute attr : optional)
      if (!llvm::is_contained(getComponents(), attr))
        return emitOpError("marks ")
               << attr << " optional but does not list it as a component";
  return success();
}

LogicalResult
ArchetypeOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  for (Attribute attr : getComponents()) {
    auto ref = cast<FlatSymbolRefAttr>(attr);
    if (!lookupComponent(symbolTable, *this, ref))
      return emitOpError("contains unknown component ") << ref;
  }
  return success();
}

bool ArchetypeOp::contains(FlatSymbolRefAttr component) {
  return llvm::is_contained(getComponents(), component);
}

bool ArchetypeOp::isOptional(FlatSymbolRefAttr component) {
  ArrayAttr optional = getOptionalAttr();
  return optional && llvm::is_contained(optional, component);
}

//===----------------------------------------------------------------------===//
// SystemOp
//===----------------------------------------------------------------------===//

// ecs.system @name(%arg: T, ...) (reads [@A, ...])? (writes [@B, ...])?
//     (attributes {...})? { body }
ParseResult SystemOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  ArrayAttr reads = parser.getBuilder().getArrayAttr({});
  ArrayAttr writes = reads;
  auto parseAccess = [&]() -> ParseResult {
    if (succeeded(parser.parseOptionalKeyword("reads")) &&
        parser.parseAttribute(reads))
      return failure();
    if (succeeded(parser.parseOptionalKeyword("writes")) &&
        parser.parseAttribute(writes))
      return failure();
    return success();
  };
  if (parseArgsAndBody<SystemOp>(parser, result, parseAccess))
    return failure();
  result.addAttribute(getReadsAttrName(result.name), reads);
  result.addAttribute(getWritesAttrName(result.name), writes);
  return success();
}

void SystemOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(getSymName());
  printArgs(p, getBody());
  if (!getReads().empty())
    p << " reads " << getReads();
  if (!getWrites().empty())
    p << " writes " << getWrites();
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(),
      {getSymNameAttrName(), getReadsAttrName(), getWritesAttrName()});
  printBody(p, getBody());
}

LogicalResult SystemOp::verify() {
  if (failed(verifyNoRefParams(*this, getBody())))
    return failure();

  llvm::SmallPtrSet<Attribute, 8> seen;
  for (auto [listName, list] :
       {std::pair<StringRef, ArrayAttr>{"reads", getReads()},
        std::pair<StringRef, ArrayAttr>{"writes", getWrites()}}) {
    for (Attribute attr : list) {
      if (!isa<FlatSymbolRefAttr>(attr))
        return emitOpError("'") << listName << "' entry " << attr
                                << " must be a flat symbol reference";
      if (!seen.insert(attr).second)
        return emitOpError("lists ")
               << attr
               << " more than once; 'writes' already implies read access";
    }
  }
  return success();
}

LogicalResult SystemOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  for (ArrayAttr list : {getReads(), getWrites()})
    for (Attribute attr : list) {
      auto ref = cast<FlatSymbolRefAttr>(attr);
      Operation *target = symbolTable.lookupNearestSymbolFrom(*this, ref);
      if (!isa_and_nonnull<ComponentOp, ResourceOp, ArchetypeOp>(target))
        return emitOpError("declares access to unknown component, resource "
                           "or archetype ")
               << ref;
      if (isa<ArchetypeOp>(target) && list == getReads())
        return emitOpError("lists archetype ")
               << ref
               << " in 'reads'; archetypes are declared in 'writes', by "
                  "systems that spawn or despawn their entities";
    }
  return success();
}

bool SystemOp::canWrite(FlatSymbolRefAttr component) {
  return llvm::is_contained(getWrites(), component);
}

bool SystemOp::canRead(FlatSymbolRefAttr component) {
  return canWrite(component) || llvm::is_contained(getReads(), component);
}

//===----------------------------------------------------------------------===//
// ScheduleOp
//===----------------------------------------------------------------------===//

ParseResult ScheduleOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();
  return parseArgsAndBody<ScheduleOp>(parser, result);
}

void ScheduleOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(getSymName());
  printArgs(p, getBody());
  p.printOptionalAttrDictWithKeyword((*this)->getAttrs(),
                                     {getSymNameAttrName()});
  printBody(p, getBody());
}

LogicalResult ScheduleOp::verify() {
  return verifyNoRefParams(*this, getBody());
}

//===----------------------------------------------------------------------===//
// StageOp
//===----------------------------------------------------------------------===//

LogicalResult StageOp::verify() {
  for (Operation &op : getBody().front().without_terminator())
    if (!isa<RunOp>(op))
      return op.emitOpError("is not allowed in 'ecs.stage'; a stage holds "
                            "only 'ecs.run'");
  return success();
}

//===----------------------------------------------------------------------===//
// QueryOp
//===----------------------------------------------------------------------===//

// `on [changed @C "f", added @C log 4096, removed @C, changed @C]`
ParseResult QueryOp::parse(OpAsmParser &parser, OperationState &result) {
  auto parseTriggers = [&]() -> ParseResult {
    if (failed(parser.parseOptionalKeyword("on")))
      return success();
    Builder &builder = parser.getBuilder();
    SmallVector<Attribute> triggers;
    auto parseOne = [&]() -> ParseResult {
      StringRef kind;
      FlatSymbolRefAttr component;
      llvm::SMLoc loc = parser.getCurrentLocation();
      if (parser.parseKeyword(&kind) || parser.parseAttribute(component))
        return failure();
      if (kind != "added" && kind != "removed" && kind != "changed")
        return parser.emitError(loc, "unknown trigger '")
               << kind << "'; expected 'added', 'removed' or 'changed'";
      std::string field;
      if (kind == "changed")
        (void)parser.parseOptionalString(&field);
      SmallVector<Attribute, 4> entry{builder.getStringAttr(kind), component,
                                      builder.getStringAttr(field)};
      if (succeeded(parser.parseOptionalKeyword("log"))) {
        int64_t capacity;
        if (parser.parseInteger(capacity))
          return failure();
        entry.push_back(builder.getI64IntegerAttr(capacity));
      }
      triggers.push_back(builder.getArrayAttr(entry));
      return success();
    };
    if (parser.parseCommaSeparatedList(OpAsmParser::Delimiter::Square,
                                       parseOne))
      return failure();
    result.addAttribute(kTriggersAttr, builder.getArrayAttr(triggers));
    return success();
  };
  return parseArgsAndBody<QueryOp>(parser, result, parseTriggers);
}

void QueryOp::print(OpAsmPrinter &p) {
  p << " ";
  printArgs(p, getBody());
  if (auto triggers = (*this)->getAttrOfType<ArrayAttr>(kTriggersAttr)) {
    p << " on [";
    llvm::interleaveComma(triggers, p, [&](Attribute attr) {
      auto entry = cast<ArrayAttr>(attr);
      p << cast<StringAttr>(entry[0]).getValue() << " " << entry[1];
      if (!cast<StringAttr>(entry[2]).getValue().empty())
        p << " " << entry[2];
      if (entry.size() > 3)
        p << " log " << cast<IntegerAttr>(entry[3]).getInt();
    });
    p << "]";
  }
  p.printOptionalAttrDictWithKeyword((*this)->getAttrs(), {kTriggersAttr});
  printBody(p, getBody());
}

LogicalResult QueryOp::verify() {
  Block &body = getBody().front();
  if (body.getNumArguments() == 0)
    return emitOpError("must bind at least one component");

  llvm::SmallPtrSet<Attribute, 8> seen;
  for (BlockArgument arg : body.getArguments()) {
    auto refType = dyn_cast<RefType>(arg.getType());
    if (!refType)
      return emitOpError("argument #")
             << arg.getArgNumber() << " must be an !ecs.ref, got "
             << arg.getType();
    // Two refs to the same component of one entity would alias.
    if (!seen.insert(refType.getComponent()).second)
      return emitOpError("binds component ")
             << refType.getComponent() << " more than once";
    // Lowering replaces refs by an index into the matched archetype's
    // columns, which only works if nothing else holds on to them.
    for (Operation *user : arg.getUsers())
      if (!isa<GetOp, SetOp>(user))
        return user->emitOpError("uses component reference #")
               << arg.getArgNumber()
               << "; references may only be used by 'ecs.get' and 'ecs.set'";
  }
  return success();
}

LogicalResult QueryOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system = (*this)->getParentOfType<SystemOp>();
  for (const Trigger &trigger : getTriggers(*this)) {
    ComponentOp component =
        lookupComponent(symbolTable, *this, trigger.component);
    if (!component)
      return emitOpError("reacts to unknown component ") << trigger.component;
    if (trigger.field && !trigger.field.getValue().empty() &&
        !component.getFieldType(trigger.field))
      return emitOpError("component ")
             << trigger.component << " has no field '"
             << trigger.field.getValue() << "'";
    if (trigger.logCapacity && *trigger.logCapacity < 0)
      return emitOpError("gives the event log of ")
             << trigger.component << " a negative capacity";
    if (!system.canRead(trigger.component))
      return emitOpError("reacts to ")
             << trigger.component << " but system @" << system.getSymName()
             << " does not declare it in 'reads' or 'writes'";
    // An entity that lost the component cannot match a query binding it.
    if (trigger.kind == Trigger::Removed &&
        llvm::any_of(getBody().getArgumentTypes(), [&](Type type) {
          return cast<RefType>(type).getComponent() == trigger.component;
        }))
      return emitOpError("reacts to removed ")
             << trigger.component
             << " but binds it; an entity that lost it never matches";
  }
  for (BlockArgument arg : getBody().getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    FlatSymbolRefAttr component = refType.getComponent();
    if (!lookupComponent(symbolTable, *this, component))
      return emitOpError("binds unknown component ") << component;
    if (refType.getIsMutable() && !system.canWrite(component))
      return emitOpError("binds ")
             << component << " mutably but system @" << system.getSymName()
             << " does not declare it in 'writes'";
    if (!system.canRead(component))
      return emitOpError("binds ")
             << component << " but system @" << system.getSymName()
             << " does not declare it in 'reads' or 'writes'";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// GetOp / SetOp
//===----------------------------------------------------------------------===//

LogicalResult GetOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveField(symbolTable, *this, getRef().getType(), getField());
  if (failed(fieldType))
    return failure();
  if (*fieldType != getResult().getType())
    return emitOpError("result type ")
           << getResult().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  return success();
}

LogicalResult SetOp::verify() {
  if (!getRef().getType().getIsMutable())
    return emitOpError("requires a mutable reference, got ")
           << getRef().getType();
  return success();
}

LogicalResult SetOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveField(symbolTable, *this, getRef().getType(), getField());
  if (failed(fieldType))
    return failure();
  if (*fieldType != getValue().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  return success();
}

//===----------------------------------------------------------------------===//
// AddOp / RemoveOp
//===----------------------------------------------------------------------===//

/// Check the rules shared by ecs.add and ecs.remove and return the
/// component: it exists, the system declares it in `writes`, and in every
/// archetype the enclosing query matches the change is possible. A move
/// needs no archetypes in `writes`: whether a change moves entities is a
/// storage decision, and the analysis derives its effect from the storage.
static FailureOr<ComponentOp>
verifyComponentChange(SymbolTableCollection &symbolTable, Operation *op,
                      FlatSymbolRefAttr componentRef, bool add) {
  ComponentOp component = lookupComponent(symbolTable, op, componentRef);
  if (!component)
    return op->emitOpError("references unknown component ") << componentRef;
  auto system = op->getParentOfType<SystemOp>();
  if (!system.canWrite(componentRef))
    return op->emitOpError("changes ")
           << componentRef << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";
  for (ArchetypeOp archetype :
       getMatchedArchetypes(op->getParentOfType<QueryOp>())) {
    if (classifyChange(archetype, componentRef, add).kind !=
        ComponentChange::NoTarget)
      continue;
    InFlightDiagnostic diag =
        op->emitOpError(add ? "adds " : "removes ")
        << componentRef << (add ? " to" : " from") << " entities of @"
        << archetype.getSymName()
        << ", but no archetype has exactly the resulting components; "
           "declare one, or make ";
    diag << componentRef << " optional in @" << archetype.getSymName();
    return diag;
  }
  return component;
}

static LogicalResult verifyInsideQuery(Operation *op) {
  if (!op->getParentOfType<QueryOp>())
    return op->emitOpError("must be inside an 'ecs.query': it changes the "
                           "entity the query visits");
  return success();
}

LogicalResult AddOp::verify() { return verifyInsideQuery(*this); }

LogicalResult AddOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<ComponentOp> component =
      verifyComponentChange(symbolTable, *this, getComponentAttr(),
                            /*add=*/true);
  if (failed(component))
    return failure();
  ArrayAttr fieldTypes = component->getFieldTypes();
  if (fieldTypes.size() != getValues().size())
    return emitOpError("initialises ")
           << getValues().size() << " fields, but " << getComponentAttr()
           << " has " << fieldTypes.size();
  for (auto [index, value, typeAttr] :
       llvm::enumerate(getValues(), fieldTypes)) {
    Type fieldType = cast<TypeAttr>(typeAttr).getValue();
    if (value.getType() != fieldType)
      return emitOpError("value #")
             << index << " has type " << value.getType() << ", but field '"
             << cast<StringAttr>(component->getFieldNames()[index]).getValue()
             << "' has type " << fieldType;
  }
  return success();
}

LogicalResult RemoveOp::verify() { return verifyInsideQuery(*this); }

LogicalResult RemoveOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  return verifyComponentChange(symbolTable, *this, getComponentAttr(),
                               /*add=*/false);
}

//===----------------------------------------------------------------------===//
// SpawnOp / DespawnOp
//===----------------------------------------------------------------------===//

LogicalResult SpawnOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ecs.system'");
  return success();
}

LogicalResult SpawnOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto archetype =
      symbolTable.lookupNearestSymbolFrom<ArchetypeOp>(*this, getArchetypeAttr());
  if (!archetype)
    return emitOpError("references unknown archetype ") << getArchetypeAttr();
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canWrite(getArchetypeAttr()))
    return emitOpError("spawns into ")
           << getArchetypeAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";

  // Every field of every non-optional component, in order.
  SmallVector<std::pair<std::string, Type>> fields;
  for (Attribute attr : archetype.getComponents()) {
    auto ref = cast<FlatSymbolRefAttr>(attr);
    if (archetype.isOptional(ref))
      continue;
    ComponentOp component = lookupComponent(symbolTable, *this, ref);
    for (auto [name, type] :
         llvm::zip(component.getFieldNames(), component.getFieldTypes()))
      fields.push_back({(ref.getValue() + "." +
                         cast<StringAttr>(name).getValue())
                            .str(),
                        cast<TypeAttr>(type).getValue()});
  }
  if (fields.size() != getValues().size())
    return emitOpError("initialises ")
           << getValues().size() << " fields, but the non-optional "
           << "components of " << getArchetypeAttr() << " have "
           << fields.size();
  for (auto [index, value, field] : llvm::enumerate(getValues(), fields))
    if (value.getType() != field.second)
      return emitOpError("value #")
             << index << " has type " << value.getType() << ", but field "
             << field.first << " has type " << field.second;
  return success();
}

LogicalResult LookupOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ecs.system'");
  return success();
}

LogicalResult LookupOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  ComponentOp component =
      lookupComponent(symbolTable, *this, getComponentAttr());
  if (!component)
    return emitOpError("references unknown component ") << getComponentAttr();
  Type fieldType = component.getFieldType(getField());
  if (!fieldType)
    return emitOpError("component ")
           << getComponentAttr() << " has no field '" << getField() << "'";
  if (fieldType != getValue().getType())
    return emitOpError("result type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << fieldType;
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canRead(getComponentAttr()))
    return emitOpError("looks up ")
           << getComponentAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'reads' or 'writes'";

  // Within a query, the looked-up field must not change: other entities'
  // values would be old or new depending on iteration order.
  auto query = (*this)->getParentOfType<QueryOp>();
  if (!query)
    return success();
  Operation *writer = nullptr;
  query.walk([&](Operation *op) {
    if (auto set = dyn_cast<SetOp>(op)) {
      if (cast<RefType>(set.getRef().getType()).getComponent() ==
              getComponentAttr() &&
          set.getField() == getField())
        writer = op;
    } else if (isa<AddOp, RemoveOp>(op) &&
               op->getAttr("component") == getComponentAttr()) {
      writer = op;
    }
  });
  if (writer) {
    InFlightDiagnostic diag = emitOpError("looks up ")
                              << getComponentAttr() << " \"" << getField()
                              << "\" of other entities in a query that "
                                 "changes it; which entities see the old "
                                 "value would depend on iteration order";
    diag.attachNote(writer->getLoc()) << "changed here";
    return diag;
  }
  return success();
}

LogicalResult ApplyOp::verify() {
  auto query = (*this)->getParentOfType<QueryOp>();
  if (!query)
    return emitOpError("must be inside an 'ecs.query': its values are "
                       "combined when the query ends");
  // At most one value per entity and apply: the buffer has one slot per row.
  for (Operation *parent = (*this)->getParentOp(); parent != query;
       parent = parent->getParentOp())
    if (isa<LoopLikeOpInterface>(parent))
      return emitOpError("must not be inside a loop ('")
             << parent->getName() << "'): it may run at most once per entity";
  StringRef rule = getRule();
  if (rule != "add" && rule != "min" && rule != "max")
    return emitOpError("has unknown rule '")
           << rule << "'; expected 'add', 'min' or 'max'";
  return success();
}

LogicalResult ApplyOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  ComponentOp component =
      lookupComponent(symbolTable, *this, getComponentAttr());
  if (!component)
    return emitOpError("references unknown component ") << getComponentAttr();
  Type fieldType = component.getFieldType(getField());
  if (!fieldType)
    return emitOpError("component ")
           << getComponentAttr() << " has no field '" << getField() << "'";
  if (fieldType != getValue().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << fieldType;
  if (!isa<FloatType>(fieldType) && !fieldType.isIntOrIndex())
    return emitOpError("cannot combine field '")
           << getField() << "' of type " << fieldType
           << "; only integers and floats can";
  if (fieldType.isInteger(1))
    return emitOpError("cannot combine field '")
           << getField() << "' of type i1; only integers and floats can";
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canWrite(getComponentAttr()))
    return emitOpError("applies to ")
           << getComponentAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";

  // One rule per field and query: mixed rules would not commute.
  ApplyOp other;
  (*this)->getParentOfType<QueryOp>().walk([&](ApplyOp apply) {
    if (!other && apply.getComponentAttr() == getComponentAttr() &&
        apply.getField() == getField() && apply.getRule() != getRule())
      other = apply;
  });
  if (other) {
    InFlightDiagnostic diag =
        emitOpError("combines ")
        << getComponentAttr() << " \"" << getField() << "\" with '"
        << getRule() << "', but the query also combines it with '"
        << other.getRule() << "'; the result would depend on the order";
    diag.attachNote(other.getLoc()) << "other rule here";
    return diag;
  }
  return success();
}

LogicalResult DespawnOp::verify() { return verifyInsideQuery(*this); }

LogicalResult EntityOp::verify() { return verifyInsideQuery(*this); }

LogicalResult DespawnOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system = (*this)->getParentOfType<SystemOp>();
  auto query = (*this)->getParentOfType<QueryOp>();
  auto module = (*this)->getParentOfType<ModuleOp>();
  for (ArchetypeOp archetype : module.getOps<ArchetypeOp>()) {
    bool matches = llvm::all_of(
        query.getBody().getArgumentTypes(), [&](Type type) {
          return archetype.contains(cast<RefType>(type).getComponent());
        });
    auto ref = FlatSymbolRefAttr::get(archetype.getSymNameAttr());
    if (matches && !system.canWrite(ref))
      return emitOpError("despawns entities of ")
             << ref << " but system @" << system.getSymName()
             << " does not declare it in 'writes'";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// ReadOp / WriteOp
//===----------------------------------------------------------------------===//

/// Resolve the resource field that a read/write names, check that the
/// enclosing system declares the access, and return the field's type.
static FailureOr<Type> resolveResourceField(SymbolTableCollection &symbolTable,
                                            Operation *op,
                                            FlatSymbolRefAttr resource,
                                            StringRef field, bool write) {
  auto resourceOp =
      symbolTable.lookupNearestSymbolFrom<ResourceOp>(op, resource);
  if (!resourceOp)
    return op->emitOpError("references unknown resource ") << resource;
  Type fieldType = resourceOp.getFieldType(field);
  if (!fieldType)
    return op->emitOpError("resource ")
           << resource << " has no field '" << field << "'";
  auto system = op->getParentOfType<SystemOp>();
  if (write && !system.canWrite(resource))
    return op->emitOpError("writes ")
           << resource << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";
  if (!system.canRead(resource))
    return op->emitOpError("reads ")
           << resource << " but system @" << system.getSymName()
           << " does not declare it in 'reads' or 'writes'";
  return fieldType;
}

LogicalResult ReadOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ecs.system'");
  return success();
}

LogicalResult ReadOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveResourceField(symbolTable, *this, getResourceAttr(), getField(),
                           /*write=*/false);
  if (failed(fieldType))
    return failure();
  if (*fieldType != getResult().getType())
    return emitOpError("result type ")
           << getResult().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  return success();
}

LogicalResult WriteOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ecs.system'");
  if ((*this)->getParentOfType<QueryOp>())
    return emitOpError("cannot write a resource inside 'ecs.query': every "
                       "entity would write the same field, which makes the "
                       "entities depend on each other; write it at system "
                       "level");
  return success();
}

LogicalResult WriteOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveResourceField(symbolTable, *this, getResourceAttr(), getField(),
                           /*write=*/true);
  if (failed(fieldType))
    return failure();
  if (*fieldType != getValue().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  return success();
}

//===----------------------------------------------------------------------===//
// RunOp
//===----------------------------------------------------------------------===//

LogicalResult RunOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system =
      symbolTable.lookupNearestSymbolFrom<SystemOp>(*this, getSystemAttr());
  if (!system)
    return emitOpError("references unknown system ") << getSystemAttr();

  TypeRange params = system.getBody().getArgumentTypes();
  if (params != getArgs().getTypes())
    return emitOpError("argument types (")
           << getArgs().getTypes() << ") do not match the parameters ("
           << params << ") of system " << getSystemAttr();
  return success();
}
