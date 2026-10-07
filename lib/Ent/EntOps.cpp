#include "Ent/EntOps.h"
#include "Ent/Structure.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace mlir;
using namespace mlir::ent;

// `(@A, optional @B)`, used by ent.archetype.
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

// A bare keyword naming how ent.apply combines values: `add`, `min`, `max`.
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

// `f32, i64`: the parameter types of ent.extern, possibly none.
static ParseResult parseParamTypes(OpAsmParser &parser, ArrayAttr &params) {
  SmallVector<Type> types;
  Type type;
  OptionalParseResult first = parser.parseOptionalType(type);
  if (first.has_value()) {
    if (failed(*first))
      return failure();
    types.push_back(type);
    while (succeeded(parser.parseOptionalComma())) {
      if (parser.parseType(type))
        return failure();
      types.push_back(type);
    }
  }
  params = parser.getBuilder().getTypeArrayAttr(types);
  return success();
}

static void printParamTypes(OpAsmPrinter &p, Operation *, ArrayAttr params) {
  llvm::interleaveComma(params.getAsValueRange<TypeAttr>(), p);
}

#define GET_OP_CLASSES
#include "Ent/EntOps.cpp.inc"

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

/// Refs may only be introduced by `ent.query`; a system or schedule taking a
/// ref parameter would smuggle component access past the declared sets.
static LogicalResult verifyNoRefParams(Operation *op, Region &body) {
  for (BlockArgument arg : body.getArguments())
    if (isa<RefType>(arg.getType()))
      return op->emitOpError("parameter #")
             << arg.getArgNumber()
             << " is a component reference; references can only be bound "
                "by 'ent.query'";
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
  // A ref names a component (bound by a query) or a relation (bound by
  // ent.edges).
  if (auto relation = symbolTable.lookupNearestSymbolFrom<RelationOp>(
          op, refType.getComponent())) {
    Type fieldType = relation.getFieldType(field);
    if (!fieldType)
      return op->emitOpError("relation ")
             << refType.getComponent() << " has no field '" << field << "'";
    return fieldType;
  }
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
  // `[from @C] [to @D] [tree] capacity N`, for relations.
  if constexpr (std::is_same_v<OpTy, RelationOp>) {
    for (StringRef end : {"from", "to"}) {
      if (failed(parser.parseOptionalKeyword(end)))
        continue;
      FlatSymbolRefAttr component;
      if (parser.parseAttribute(component))
        return failure();
      result.addAttribute(end, component);
    }
    if (succeeded(parser.parseOptionalKeyword("tree")))
      result.addAttribute(OpTy::getTreeAttrName(result.name),
                          b.getUnitAttr());
    if (succeeded(parser.parseOptionalKeyword("sorted")))
      result.addAttribute(OpTy::getSortedAttrName(result.name),
                          b.getUnitAttr());
    if (succeeded(parser.parseOptionalKeyword("ordered"))) {
      FlatSymbolRefAttr component;
      std::string field;
      if (parser.parseKeyword("by") || parser.parseAttribute(component) ||
          parser.parseString(&field))
        return failure();
      result.addAttribute(OpTy::getOrderComponentAttrName(result.name),
                          component);
      result.addAttribute(OpTy::getOrderFieldAttrName(result.name),
                          b.getStringAttr(field));
    }
    int64_t capacity;
    if (parser.parseKeyword("capacity") || parser.parseInteger(capacity))
      return failure();
    result.addAttribute(OpTy::getCapacityAttrName(result.name),
                        b.getI64IntegerAttr(capacity));
  }
  // `capacity N`, for components.
  if constexpr (std::is_same_v<OpTy, ComponentOp>) {
    if (succeeded(parser.parseOptionalKeyword("capacity"))) {
      int64_t capacity;
      if (parser.parseInteger(capacity))
        return failure();
      result.addAttribute(OpTy::getCapacityAttrName(result.name),
                          b.getI64IntegerAttr(capacity));
    }
  }
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
  SmallVector<StringRef, 4> elided{op.getSymNameAttrName(),
                                   op.getFieldNamesAttrName(),
                                   op.getFieldTypesAttrName()};
  if constexpr (std::is_same_v<OpTy, ComponentOp>) {
    if (std::optional<int64_t> capacity = op.getCapacity())
      p << " capacity " << *capacity;
    elided.push_back(op.getCapacityAttrName());
  }
  if constexpr (std::is_same_v<OpTy, RelationOp>) {
    if (FlatSymbolRefAttr from = op.getFromAttr())
      p << " from " << from;
    if (FlatSymbolRefAttr to = op.getToAttr())
      p << " to " << to;
    if (op.getTree())
      p << " tree";
    if (op.getSorted())
      p << " sorted";
    if (op.isOrdered())
      p << " ordered by " << op.getOrderComponentAttr() << " "
        << op.getOrderFieldAttr();
    p << " capacity " << op.getCapacity();
    elided.append({op.getCapacityAttrName(), op.getFromAttrName(),
                   op.getToAttrName(), op.getTreeAttrName(),
                   op.getSortedAttrName(), op.getOrderComponentAttrName(),
                   op.getOrderFieldAttrName()});
  }
  p.printOptionalAttrDict(op->getAttrs(), elided);
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
    if (!isa<IntegerType, FloatType, IndexType, EntityType, TextType,
             EnumType>(type))
      return op->emitOpError("field '")
             << name << "' has type " << type
             << "; only integer, float, index, entity, text and enum fields "
                "are supported";
    if (auto text = dyn_cast<TextType>(type))
      if (text.getCapacity() == 0 || text.getCapacity() > TextType::kMaxCapacity)
        return op->emitOpError("field '")
               << name << "' is a text of capacity " << text.getCapacity()
               << "; a text holds 1 to " << TextType::kMaxCapacity
               << " bytes";
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

ParseResult RelationOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseRecord<RelationOp>(parser, result);
}
void RelationOp::print(OpAsmPrinter &p) { printRecord(*this, p); }
LogicalResult RelationOp::verify() {
  // Entities are stored in a tree's order, and the tree must be known to
  // live in the archetypes that are.
  if (getSorted()) {
    if (!getTree())
      return emitOpError("is 'sorted' but not a 'tree'; only a tree gives "
                         "its entities an order");
    for (bool target : {false, true})
      if (!getEndpoint(target))
        return emitOpError("is 'sorted' but does not say what its ")
               << (target ? "targets" : "sources") << " have ('"
               << (target ? "to" : "from")
               << "'): which archetypes it sorts must be known";
  }
  if (isOrdered()) {
    if (!getTree())
      return emitOpError("is 'ordered' but not a 'tree'; only a tree's "
                         "entities have siblings to be in an order");
    if (getSorted())
      return emitOpError("is 'sorted' and 'ordered', which is not supported "
                         "yet");
    if (!getOrderFieldAttr())
      return emitOpError("is ordered by a component but names no field");
  }
  return verifyRecord(*this, getFieldNames(), getFieldTypes());
}
Type RelationOp::getFieldType(StringRef name) {
  return lookupFieldType(getFieldNames(), getFieldTypes(), name);
}
LogicalResult RelationOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  for (bool target : {false, true})
    if (FlatSymbolRefAttr component = getEndpoint(target))
      if (!lookupComponent(symbolTable, *this, component))
        return emitOpError("names unknown component ")
               << component << " for its " << (target ? "targets" : "sources");
  if (isOrdered()) {
    ComponentOp component =
        lookupComponent(symbolTable, *this, getOrderComponentAttr());
    if (!component)
      return emitOpError("is ordered by unknown component ")
             << getOrderComponentAttr();
    Type type = component.getFieldType(*getOrderField());
    if (!type)
      return emitOpError("is ordered by ")
             << getOrderComponentAttr() << " \"" << *getOrderField()
             << "\", but the component has no such field";
    if (!isa<IntegerType>(type) || type.isInteger(1))
      return emitOpError("is ordered by a field of type ")
             << type << "; the children are ordered by an integer";
  }
  return success();
}

LogicalResult BoundOp::verify() {
  if (!getRef().getType().getIsOptional())
    return emitOpError("asks whether a ref leads anywhere that always does: "
                       "only a ref 'optional' up or before a tree may not");
  return success();
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

/// A run or schedule condition: empty, or one block of resource reads and
/// ops free of side effects, ending in `ent.yield` of an i1.
static LogicalResult verifyCondition(Operation *op, Region &condition) {
  if (condition.empty())
    return success();
  auto yield = dyn_cast<YieldOp>(condition.front().getTerminator());
  if (!yield || yield.getResults().size() != 1 ||
      !yield.getResults()[0].getType().isInteger(1))
    return op->emitOpError("condition must end in 'ent.yield' of an i1");
  for (Operation &nested : condition.front().without_terminator()) {
    if (isa<ReadOp>(nested))
      continue;
    if (nested.getNumRegions() == 0 && isMemoryEffectFree(&nested))
      continue;
    return nested.emitOpError("is not allowed in a condition; a condition "
                              "reads resources and computes with ops free "
                              "of side effects");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SystemOp
//===----------------------------------------------------------------------===//

// ent.system @name(%arg: T, ...) (reads [@A, ...])? (writes [@B, ...])?
//     (attributes {...})? { body }
ParseResult SystemOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  // Declared access is optional; either list makes it a contract.
  ArrayAttr reads, writes;
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
  if (reads)
    result.addAttribute(getReadsAttrName(result.name), reads);
  if (writes)
    result.addAttribute(getWritesAttrName(result.name), writes);
  return success();
}

void SystemOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(getSymName());
  printArgs(p, getBody());
  if (ArrayAttr reads = getReadsAttr())
    p << " reads " << reads;
  if (ArrayAttr writes = getWritesAttr())
    p << " writes " << writes;
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(),
      {getSymNameAttrName(), getReadsAttrName(), getWritesAttrName()});
  printBody(p, getBody());
}

/// The `reads` and `writes` lists of a system or an extern system: flat
/// symbol references, none twice.
static LogicalResult verifyAccessLists(Operation *op, ArrayAttr reads,
                                       ArrayAttr writes) {
  llvm::SmallPtrSet<Attribute, 8> seen;
  for (auto [listName, list] : {std::pair<StringRef, ArrayAttr>{"reads", reads},
                                std::pair<StringRef, ArrayAttr>{"writes", writes}}) {
    if (!list)
      continue;
    for (Attribute attr : list) {
      if (!isa<FlatSymbolRefAttr>(attr))
        return op->emitOpError("'") << listName << "' entry " << attr
                                    << " must be a flat symbol reference";
      if (!seen.insert(attr).second)
        return op->emitOpError("lists ")
               << attr
               << " more than once; 'writes' already implies read access";
    }
  }
  return success();
}

/// What those lists may name.
static LogicalResult verifyAccessSymbols(SymbolTableCollection &symbolTable,
                                         Operation *op, ArrayAttr reads,
                                         ArrayAttr writes) {
  for (ArrayAttr list : {reads, writes}) {
    if (!list)
      continue;
    for (Attribute attr : list) {
      auto ref = cast<FlatSymbolRefAttr>(attr);
      Operation *target = symbolTable.lookupNearestSymbolFrom(op, ref);
      if (!isa_and_nonnull<ComponentOp, ResourceOp, ArchetypeOp, RelationOp>(
              target))
        return op->emitOpError("declares access to unknown component, "
                               "resource, relation "
                               "or archetype ")
               << ref;
      if (isa<ArchetypeOp>(target) && list == reads)
        return op->emitOpError("lists archetype ")
               << ref
               << " in 'reads'; archetypes are declared in 'writes', by "
                  "systems that spawn or despawn their entities";
    }
  }
  return success();
}

LogicalResult SystemOp::verify() {
  if (failed(verifyNoRefParams(*this, getBody())))
    return failure();
  return verifyAccessLists(*this, getReadsAttr(), getWritesAttr());
}

LogicalResult SystemOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  return verifyAccessSymbols(symbolTable, *this, getReadsAttr(),
                             getWritesAttr());
}

bool SystemOp::hasContract() { return getReadsAttr() || getWritesAttr(); }

bool SystemOp::canWrite(FlatSymbolRefAttr component) {
  if (!hasContract())
    return true;
  ArrayAttr writes = getWritesAttr();
  return writes && llvm::is_contained(writes, component);
}

bool SystemOp::canRead(FlatSymbolRefAttr component) {
  if (canWrite(component))
    return true;
  ArrayAttr reads = getReadsAttr();
  return reads && llvm::is_contained(reads, component);
}

//===----------------------------------------------------------------------===//
// ScheduleOp
//===----------------------------------------------------------------------===//

// ent.schedule @name(%arg: T, ...) (if { ^bb0(%arg: T, ...): ... })?
//     (attributes {...})? { body }
ParseResult ScheduleOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();
  // The condition is the second region; parse it into a holder until the
  // body is added.
  auto condition = std::make_unique<Region>();
  auto parseCondition = [&]() -> ParseResult {
    if (failed(parser.parseOptionalKeyword("if")))
      return success();
    return parser.parseRegion(*condition);
  };
  if (parseArgsAndBody<ScheduleOp>(parser, result, parseCondition))
    return failure();
  result.addRegion(std::move(condition));
  return success();
}

void ScheduleOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(getSymName());
  printArgs(p, getBody());
  if (!getCondition().empty()) {
    p << " if ";
    p.printRegion(getCondition(), /*printEntryBlockArgs=*/true,
                  /*printBlockTerminators=*/true);
  }
  p.printOptionalAttrDictWithKeyword((*this)->getAttrs(),
                                     {getSymNameAttrName()});
  printBody(p, getBody());
}

LogicalResult ScheduleOp::verify() {
  if (failed(verifyNoRefParams(*this, getBody())))
    return failure();
  Region &condition = getCondition();
  if (!condition.empty() &&
      condition.front().getArgumentTypes() != getBody().getArgumentTypes())
    return emitOpError("condition must take the schedule's parameters");
  return verifyCondition(*this, condition);
}

//===----------------------------------------------------------------------===//
// ExternOp
//===----------------------------------------------------------------------===//

LogicalResult ExternOp::verify() {
  for (auto [index, type] :
       llvm::enumerate(getParams().getAsValueRange<TypeAttr>())) {
    if (isa<RefType>(type))
      return emitOpError("parameter #")
             << index
             << " is a component reference; references can only be bound "
                "by 'ent.query'";
    if (isa<TextType>(type))
      return emitOpError("parameter #")
             << index
             << " is a text, which cannot be passed to C yet; put it in a "
                "component or resource the system reads";
  }
  return verifyAccessLists(*this, getReadsAttr(), getWritesAttr());
}

LogicalResult ExternOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  return verifyAccessSymbols(symbolTable, *this, getReadsAttr(),
                             getWritesAttr());
}

//===----------------------------------------------------------------------===//
// FunctionOp, InvokeOp
//===----------------------------------------------------------------------===//

/// The types a value can cross to C with.
static bool crossesToC(Type type) {
  return isa<FloatType, TextType, EnumType>(type) || type.isSignlessInteger();
}

//===----------------------------------------------------------------------===//
// EnumOp
//===----------------------------------------------------------------------===//

LogicalResult EnumOp::verify() {
  if (getCases().empty())
    return emitOpError("has no cases");
  if (getCases().size() > EnumType::kMaxCases)
    return emitOpError("has ")
           << getCases().size() << " cases; an enum has at most "
           << unsigned(EnumType::kMaxCases);
  llvm::StringSet<> seen;
  for (StringRef name : getCases().getAsValueRange<StringAttr>())
    if (!seen.insert(name).second)
      return emitOpError("has duplicate case '") << name << "'";
  return success();
}

// [proc] @name(T, U) [-> R]
// @name(%a: T, %b: U) -> R { body }
ParseResult FunctionOp::parse(OpAsmParser &parser, OperationState &result) {
  Builder &builder = parser.getBuilder();
  if (succeeded(parser.parseOptionalKeyword("proc")))
    result.addAttribute(getProcAttrName(result.name), builder.getUnitAttr());
  StringAttr name;
  if (parser.parseSymbolName(name, SymbolTable::getSymbolAttrName(),
                             result.attributes) ||
      parser.parseLParen())
    return failure();
  // Named parameters belong to a body, bare types to a declaration.
  SmallVector<OpAsmParser::Argument> args;
  SmallVector<Type> types;
  if (failed(parser.parseOptionalRParen())) {
    do {
      OpAsmParser::Argument arg;
      OptionalParseResult named =
          parser.parseOptionalArgument(arg, /*allowType=*/true);
      if (named.has_value()) {
        if (failed(*named))
          return failure();
        args.push_back(arg);
        types.push_back(arg.type);
      } else {
        Type type;
        if (parser.parseType(type))
          return failure();
        types.push_back(type);
      }
    } while (succeeded(parser.parseOptionalComma()));
    if (parser.parseRParen())
      return failure();
  }
  if (!args.empty() && args.size() != types.size())
    return parser.emitError(parser.getNameLoc(),
                            "names all of its parameters or none");
  result.addAttribute(getParamsAttrName(result.name),
                      builder.getTypeArrayAttr(types));
  // `-> T`, or `-> (T, U)` for several values.
  SmallVector<Type> results;
  if (succeeded(parser.parseOptionalArrow())) {
    if (succeeded(parser.parseOptionalLParen())) {
      if (parser.parseTypeList(results) || parser.parseRParen())
        return failure();
    } else {
      Type type;
      if (parser.parseType(type))
        return failure();
      results.push_back(type);
    }
  }
  result.addAttribute(getResultsAttrName(result.name),
                      builder.getTypeArrayAttr(results));
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();
  Region *body = result.addRegion();
  OptionalParseResult parsed = parser.parseOptionalRegion(*body, args);
  if (parsed.has_value() && failed(*parsed))
    return failure();
  if (!parsed.has_value() && !args.empty())
    return parser.emitError(parser.getNameLoc(),
                            "names its parameters, so it needs a body");
  return success();
}

void FunctionOp::print(OpAsmPrinter &p) {
  if (getProc())
    p << " proc";
  p << " ";
  p.printSymbolName(getSymName());
  if (isDefined()) {
    printArgs(p, getBody());
  } else {
    p << "(";
    printParamTypes(p, *this, getParams());
    p << ")";
  }
  SmallVector<Type> results = getResultTypes();
  if (results.size() == 1)
    p << " -> " << results.front();
  else if (!results.empty())
    p << " -> (" << results << ")";
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(), {getSymNameAttrName(), getParamsAttrName(),
                            getResultsAttrName(), getProcAttrName()});
  if (isDefined()) {
    p << " ";
    p.printRegion(getBody(), /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
  }
}

LogicalResult FunctionOp::verify() {
  if (isDefined()) {
    // The program's own: any value may go in and come out.
    if (getProc())
      return emitOpError("is a proc with a body, which is not supported yet");
    if (getResults().empty())
      return emitOpError("has a body, so it gives a value ('-> type')");
    if (!getBody().hasOneBlock())
      return emitOpError("body must be one block");
    SmallVector<Type> params(getParams().getAsValueRange<TypeAttr>());
    if (TypeRange(params) != TypeRange(getBody().front().getArgumentTypes()))
      return emitOpError("body's arguments do not match the parameters (")
             << params << ")";
    return verifyNoRefParams(*this, getBody());
  }
  for (auto [index, type] :
       llvm::enumerate(getParams().getAsValueRange<TypeAttr>()))
    if (!crossesToC(type))
      return emitOpError("parameter #")
             << index << " has type " << type
             << ", which cannot be passed to C";
  if (getResults().size() > 1)
    return emitOpError("gives several values, which C cannot give back");
  if (std::optional<Type> result = getResult()) {
    if (isa<TextType>(*result))
      return emitOpError("gives a text, which C cannot give back yet");
    if (!crossesToC(*result))
      return emitOpError("gives a ")
             << *result << ", which C cannot give back";
  }
  return success();
}

LogicalResult FunctionOp::verifyRegions() {
  if (!isDefined())
    return success();
  auto yield = dyn_cast<YieldOp>(&getBody().front().back());
  SmallVector<Type> results = getResultTypes();
  if (!yield || TypeRange(results) != yield.getResults().getTypes()) {
    InFlightDiagnostic diag =
        emitOpError("body must end with 'ent.yield' of ");
    if (results.size() == 1)
      return diag << "one " << results.front() << ", the function's value";
    return diag << "(" << results << "), the function's values";
  }
  // Only computing: nothing of the world, nothing that acts.
  LogicalResult result = success();
  getBody().walk([&](Operation *op) {
    if (op == yield.getOperation() ||
        op->getName().getDialectNamespace() != "ent")
      return;
    auto invoke = dyn_cast<InvokeOp>(op);
    if (invoke && !invoke.getProc())
      return;
    if (succeeded(result))
      result = op->emitOpError(invoke ? "calls a proc in a function's body; "
                                        "a function only computes"
                                      : "cannot be in a function's body; a "
                                        "function only computes from its "
                                        "parameters");
  });
  return result;
}

LogicalResult InvokeOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>() &&
      !(*this)->getParentOfType<FunctionOp>())
    return emitOpError("must be inside 'ent.system' or a function's body; "
                       "conditions and the entry point only read uniques "
                       "and compute");
  return success();
}

LogicalResult InvokeOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto function =
      symbolTable.lookupNearestSymbolFrom<FunctionOp>(*this, getCalleeAttr());
  if (!function)
    return emitOpError("references unknown function ") << getCalleeAttr();
  SmallVector<Type> params(function.getParams().getAsValueRange<TypeAttr>());
  if (TypeRange(params) != getArgs().getTypes())
    return emitOpError("argument types (")
           << getArgs().getTypes() << ") do not match the parameters ("
           << params << ") of function " << getCalleeAttr();
  SmallVector<Type> declared = function.getResultTypes();
  if (TypeRange(declared) != getResults().getTypes())
    return emitOpError("result does not match what function ")
           << getCalleeAttr() << " gives";
  if (function.getProc() != getProc())
    return emitOpError(getProc() ? "is marked 'proc', but " : "calls ")
           << getCalleeAttr()
           << (getProc() ? " is not one" : ", a proc, without 'proc'");
  return success();
}

void InvokeOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  // A proc acts on the outside, which no resource here stands for.
  if (!getProc())
    return;
  effects.emplace_back(MemoryEffects::Read::get());
  effects.emplace_back(MemoryEffects::Write::get());
}

//===----------------------------------------------------------------------===//
// MainOp, CallOp, LoopOp
//===----------------------------------------------------------------------===//

/// The body of the entry point or of a loop in it: schedule calls, loops,
/// resource reads and ops free of side effects.
static LogicalResult verifyEntryBody(Block &body) {
  for (Operation &op : body.without_terminator()) {
    if (isa<CallOp, LoopOp, ReadOp>(op))
      continue;
    if (op.getNumRegions() == 0 && isMemoryEffectFree(&op))
      continue;
    return op.emitOpError("is not allowed in 'ent.main'; the entry point "
                          "calls schedules, loops, reads resources and "
                          "computes with ops free of side effects");
  }
  return success();
}

LogicalResult MainOp::verify() {
  for (Operation *op = getOperation()->getPrevNode(); op;
       op = op->getPrevNode())
    if (isa<MainOp>(op)) {
      InFlightDiagnostic diag =
          emitOpError("is the second entry point; a program has one");
      diag.attachNote(op->getLoc()) << "the other is here";
      return diag;
    }
  if (cast<YieldOp>(getBody().front().getTerminator()).getNumOperands() != 0)
    return emitOpError("body must not yield a value");
  return verifyEntryBody(getBody().front());
}

LogicalResult LoopOp::verify() {
  auto yield = cast<YieldOp>(getBody().front().getTerminator());
  if (yield.getNumOperands() > 1 ||
      (yield.getNumOperands() == 1 &&
       !yield.getResults()[0].getType().isInteger(1)))
    return emitOpError("body must end in 'ent.yield' of nothing (repeat "
                       "forever) or of an i1 (stop if true)");
  return verifyEntryBody(getBody().front());
}

LogicalResult CallOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto schedule =
      symbolTable.lookupNearestSymbolFrom<ScheduleOp>(*this, getScheduleAttr());
  if (!schedule)
    return emitOpError("references unknown schedule ") << getScheduleAttr();
  TypeRange params = schedule.getBody().getArgumentTypes();
  if (params != getArgs().getTypes())
    return emitOpError("argument types (")
           << getArgs().getTypes() << ") do not match the parameters ("
           << params << ") of schedule " << getScheduleAttr();
  return success();
}

//===----------------------------------------------------------------------===//
// StageOp
//===----------------------------------------------------------------------===//

LogicalResult StageOp::verify() {
  for (Operation &op : getBody().front().without_terminator())
    if (!isa<RunOp>(op))
      return op.emitOpError("is not allowed in 'ent.stage'; a stage holds "
                            "only 'ent.run'");
  return success();
}

//===----------------------------------------------------------------------===//
// QueryOp
//===----------------------------------------------------------------------===//

// `with [@A] without [@B] any [@C, @D] any [@E, @F]`, then
// `cascade @R`, then
// `on [changed @C "f", added @C log 4096, removed @C, changed @C]`
ParseResult QueryOp::parse(OpAsmParser &parser, OperationState &result) {
  auto parseTriggers = [&]() -> ParseResult {
    Builder &b = parser.getBuilder();
    ArrayAttr with, without;
    SmallVector<Attribute> groups;
    if (succeeded(parser.parseOptionalKeyword("with")) &&
        parser.parseAttribute(with))
      return failure();
    if (succeeded(parser.parseOptionalKeyword("without")) &&
        parser.parseAttribute(without))
      return failure();
    while (succeeded(parser.parseOptionalKeyword("any"))) {
      ArrayAttr group;
      if (parser.parseAttribute(group))
        return failure();
      groups.push_back(group);
    }
    if (with)
      result.addAttribute(kWithAttr, with);
    if (without)
      result.addAttribute(kWithoutAttr, without);
    if (!groups.empty())
      result.addAttribute(kAnyAttr, b.getArrayAttr(groups));
    if (succeeded(parser.parseOptionalKeyword("cascade"))) {
      FlatSymbolRefAttr relation;
      if (parser.parseAttribute(relation))
        return failure();
      result.addAttribute(kCascadeAttr, relation);
      if (succeeded(parser.parseOptionalKeyword("leaves"))) {
        if (parser.parseKeyword("first"))
          return failure();
        result.addAttribute(kLeavesFirstAttr, b.getUnitAttr());
      }
    }
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
      if (succeeded(parser.parseOptionalKeyword("up"))) {
        FlatSymbolRefAttr via;
        if (parser.parseAttribute(via))
          return failure();
        entry.push_back(via);
      }
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
  if (auto with = (*this)->getAttrOfType<ArrayAttr>(kWithAttr))
    p << " with " << with;
  if (auto without = (*this)->getAttrOfType<ArrayAttr>(kWithoutAttr))
    p << " without " << without;
  if (auto groups = (*this)->getAttrOfType<ArrayAttr>(kAnyAttr))
    for (Attribute group : groups)
      p << " any " << group;
  if (FlatSymbolRefAttr cascade = getCascade())
    p << " cascade " << cascade << (isLeavesFirst() ? " leaves first" : "");
  if (auto triggers = (*this)->getAttrOfType<ArrayAttr>(kTriggersAttr)) {
    p << " on [";
    llvm::interleaveComma(triggers, p, [&](Attribute attr) {
      auto entry = cast<ArrayAttr>(attr);
      p << cast<StringAttr>(entry[0]).getValue() << " " << entry[1];
      if (!cast<StringAttr>(entry[2]).getValue().empty())
        p << " " << entry[2];
      for (Attribute extra : entry.getValue().drop_front(3)) {
        if (auto capacity = dyn_cast<IntegerAttr>(extra))
          p << " log " << capacity.getInt();
        else
          p << " up " << extra;
      }
    });
    p << "]";
  }
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(),
      {kTriggersAttr, kWithAttr, kWithoutAttr, kAnyAttr, kCascadeAttr,
       kLeavesFirstAttr});
  printBody(p, getBody());
}

/// `attr` as a list of components, or failure if it is not one.
static FailureOr<SmallVector<FlatSymbolRefAttr>>
componentList(Attribute attr) {
  auto list = dyn_cast<ArrayAttr>(attr);
  if (!list)
    return failure();
  SmallVector<FlatSymbolRefAttr> components;
  for (Attribute entry : list) {
    auto ref = dyn_cast<FlatSymbolRefAttr>(entry);
    if (!ref)
      return failure();
    components.push_back(ref);
  }
  return components;
}

SmallVector<FlatSymbolRefAttr> QueryOp::getRequired() {
  SmallVector<FlatSymbolRefAttr> required;
  for (Type type : getBody().getArgumentTypes())
    if (auto ref = dyn_cast<RefType>(type); ref && !ref.isUp())
      required.push_back(ref.getComponent());
  if (Attribute with = (*this)->getAttr(kWithAttr))
    if (auto list = componentList(with); succeeded(list))
      required.append(list->begin(), list->end());
  return required;
}

bool QueryOp::hasUpRefs() {
  return llvm::any_of(getBody().getArgumentTypes(), [](Type type) {
    auto ref = dyn_cast<RefType>(type);
    return ref && ref.isUp();
  });
}

SmallVector<FlatSymbolRefAttr> QueryOp::getWithout() {
  if (Attribute without = (*this)->getAttr(kWithoutAttr))
    if (auto list = componentList(without); succeeded(list))
      return *list;
  return {};
}

SmallVector<SmallVector<FlatSymbolRefAttr>> QueryOp::getAnyGroups() {
  SmallVector<SmallVector<FlatSymbolRefAttr>> groups;
  if (auto list = (*this)->getAttrOfType<ArrayAttr>(kAnyAttr))
    for (Attribute group : list)
      if (auto components = componentList(group); succeeded(components))
        groups.push_back(*components);
  return groups;
}

LogicalResult QueryOp::verify() {
  Block &body = getBody().front();
  for (StringRef name : {kWithAttr, kWithoutAttr})
    if (Attribute attr = (*this)->getAttr(name))
      if (failed(componentList(attr)))
        return emitOpError("'") << name << "' must be a list of components";
  if (Attribute attr = (*this)->getAttr(kAnyAttr)) {
    auto groups = dyn_cast<ArrayAttr>(attr);
    if (!groups)
      return emitOpError("'any' must be a list of component lists");
    for (Attribute group : groups) {
      FailureOr<SmallVector<FlatSymbolRefAttr>> list = componentList(group);
      if (failed(list))
        return emitOpError("'any' must be a list of component lists");
      if (list->size() < 2)
        return emitOpError("an 'any' group needs at least two components; "
                           "use 'with' for one");
    }
  }
  for (BlockArgument arg : body.getArguments())
    if (!isa<RefType>(arg.getType()))
      return emitOpError("argument #")
             << arg.getArgNumber() << " must be an !ent.ref, got "
             << arg.getType();
  if (getRequired().empty() && getWithout().empty() && getAnyGroups().empty())
    return emitOpError("must bind or filter by at least one component");

  // Every component appears in at most one term: two would be redundant
  // (with, any) or contradict each other (without).
  llvm::SmallPtrSet<Attribute, 8> terms;
  auto addTerm = [&](FlatSymbolRefAttr component) -> LogicalResult {
    if (!terms.insert(component).second)
      return emitOpError("names component ")
             << component << " in more than one binding or filter";
    return success();
  };
  for (FlatSymbolRefAttr component : getWithout())
    if (failed(addTerm(component)))
      return failure();
  for (auto &group : getAnyGroups())
    for (FlatSymbolRefAttr component : group)
      if (failed(addTerm(component)))
        return failure();
  if (auto with = (*this)->getAttr(kWithAttr))
    for (FlatSymbolRefAttr component : *componentList(with))
      if (failed(addTerm(component)))
        return failure();

  if (Attribute attr = (*this)->getAttr(kCascadeAttr))
    if (!isa<FlatSymbolRefAttr>(attr))
      return emitOpError("'cascade' must name a relation");
  if (isLeavesFirst() && !getCascade())
    return emitOpError("is 'leaves first' without a 'cascade'");
  llvm::SmallPtrSet<Attribute, 8> seen;
  llvm::SmallDenseSet<std::pair<Attribute, Attribute>, 4> seenUp, seenBefore;
  for (BlockArgument arg : body.getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    if (!refType.isUp() && (refType.getIsBefore() || refType.getIsOptional()))
      return emitOpError("argument #")
             << arg.getArgNumber() << " is " << refType
             << "; only a ref up or before a tree can be that";
    if (refType.getIsBefore() && refType.getIsMutable())
      return emitOpError("binds ")
             << refType << "; the sibling before is only read";
    if (refType.isUp()) {
      // An ancestor's component (or a sibling's): another entity's, so it
      // may also be bound or filtered by for the entity itself.
      if (!(refType.getIsBefore() ? seenBefore : seenUp)
               .insert({refType.getComponent(), refType.getVia()})
               .second)
        return emitOpError("binds component ")
               << refType.getComponent()
               << (refType.getIsBefore() ? " before " : " up ")
               << refType.getVia() << " more than once";
      for (Operation *user : arg.getUsers())
        if (!isa<GetOp, CombineOp, BoundOp>(user))
          return user->emitOpError("uses component reference #")
                 << arg.getArgNumber()
                 << "; a reference 'up' a relation may only be used by "
                    "'ent.get' and 'ent.combine'";
      continue;
    }
    // Two refs to the same component of one entity would alias.
    if (!seen.insert(refType.getComponent()).second)
      return emitOpError("binds component ")
             << refType.getComponent() << " more than once";
    if (terms.contains(refType.getComponent()))
      return emitOpError("names component ")
             << refType.getComponent()
             << " in more than one binding or filter";
    // Lowering replaces refs by an index into the matched archetype's
    // columns, which only works if nothing else holds on to them.
    for (Operation *user : arg.getUsers())
      if (!isa<GetOp, SetOp>(user))
        return user->emitOpError("uses component reference #")
               << arg.getArgNumber()
               << "; references may only be used by 'ent.get' and 'ent.set'";
  }

  // A cascading query runs as one query per depth. What it sends (applies,
  // accumulates) lands when the depth that sent it is through; it despawns
  // when all of it has run, like any query. What else changes which
  // entities there are, or the tree, has no place in it yet.
  if (getCascade()) {
    // Nothing of the query may see what a depth sends part way: an entity
    // of the same depth would see what those visited before it sent.
    Operation *sender = nullptr, *seer = nullptr;
    getBody().walk([&](Operation *op) {
      if (sender)
        return;
      if (auto accumulate = dyn_cast<AccumulateOp>(op)) {
        getBody().walk([&](ReadOp read) {
          if (!seer && read.getResourceAttr() == accumulate.getResourceAttr() &&
              read.getField() == accumulate.getField())
            seer = read;
        });
      } else if (auto apply = dyn_cast<ApplyOp>(op)) {
        getBody().walk([&](Operation *other) {
          if (seer)
            return;
          FlatSymbolRefAttr component;
          StringRef field;
          if (auto get = dyn_cast<GetOp>(other)) {
            component = get.getRef().getType().getComponent();
            field = get.getField();
          } else if (auto set = dyn_cast<SetOp>(other)) {
            component = set.getRef().getType().getComponent();
            field = set.getField();
          } else if (auto lookup = dyn_cast<LookupOp>(other)) {
            component = lookup.getComponentAttr();
            field = lookup.getField();
          } else if (isa<AddOp>(other)) {
            // (which sets every field of its component)
            if (other->getAttr("component") == apply.getComponentAttr())
              seer = other;
            return;
          } else {
            return;
          }
          if (component == apply.getComponentAttr() &&
              field == apply.getField())
            seer = other;
        });
      }
      if (seer)
        sender = op;
    });
    if (sender) {
      InFlightDiagnostic diag =
          sender->emitOpError("in a cascading query combines into a field "
                              "the query also reads or sets; what an entity "
                              "sees of it would depend on the order within "
                              "its depth");
      diag.attachNote(seer->getLoc()) << "read or set here";
      return diag;
    }
  }

  // Like a lookup, a ref to an ancestor reads another entity: the query
  // must not change what it reads, unless it visits ancestors first.
  for (BlockArgument arg : body.getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    if (!refType.isUp())
      continue;
    // (The sibling before is visited first where parents are.)
    bool ordered = getCascade() == refType.getVia() &&
                   !(refType.getIsBefore() && isLeavesFirst());
    Operation *writer = nullptr;
    StringRef what;
    getBody().walk([&](Operation *op) {
      if (writer)
        return;
      if (isa<AddOp, RemoveOp>(op) &&
          op->getAttr("component") == refType.getComponent()) {
        writer = op;
        return;
      }
      auto set = dyn_cast<SetOp>(op);
      if (ordered || !set)
        return;
      auto target = dyn_cast<RefType>(set.getRef().getType());
      if (!target || target.getComponent() != refType.getComponent())
        return;
      for (Operation *user : arg.getUsers())
        if (auto get = dyn_cast<GetOp>(user))
          if (get.getField() == set.getField()) {
            writer = op;
            what = get.getField();
          }
    });
    if (writer) {
      InFlightDiagnostic diag = emitOpError("reads ") << refType.getComponent();
      if (!what.empty())
        diag << " \"" << what << "\"";
      diag << (refType.getIsBefore() ? " of the sibling before in "
                                     : " of an ancestor up ")
           << refType.getVia()
           << " and changes it; which entities see the old value would "
              "depend on iteration order";
      if (!what.empty())
        diag << " ('cascade " << refType.getVia()
             << (refType.getIsBefore()
                     ? "' visits siblings in order, parents first)"
                     : "' visits ancestors first)");
      diag.attachNote(writer->getLoc()) << "changed here";
      return diag;
    }
  }
  return success();
}

LogicalResult QueryOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system = (*this)->getParentOfType<SystemOp>();
  for (const Trigger &trigger : getTriggers(*this)) {
    // (Brought along by a trigger up a tree, which is checked itself.)
    if (trigger.kind == Trigger::Connected)
      continue;
    ComponentOp component =
        lookupComponent(symbolTable, *this, trigger.component);
    if (!component)
      return emitOpError("reacts to unknown component ") << trigger.component;
    if (trigger.field && !trigger.field.getValue().empty() &&
        !component.getFieldType(trigger.field))
      return emitOpError("component ")
             << trigger.component << " has no field '"
             << trigger.field.getValue() << "'";
    if (trigger.via) {
      // An ancestor's event: of the component a ref up that tree binds.
      if (trigger.kind != Trigger::Changed)
        return emitOpError("reacts to an ancestor gaining or losing ")
               << trigger.component << "; only 'changed' can be 'up' a tree";
      bool bound = llvm::any_of(getBody().getArgumentTypes(), [&](Type type) {
        auto ref = cast<RefType>(type);
        return ref.getVia() == trigger.via && !ref.getIsBefore() &&
               ref.getComponent() == trigger.component;
      });
      if (getCascade() != trigger.via)
        return emitOpError("reacts to changed ")
               << trigger.component << " up " << trigger.via
               << " without 'cascade " << trigger.via
               << "': an ancestor is seen to change where it is visited "
               << "first";
      if (!bound)
        return emitOpError("reacts to changed ")
               << trigger.component << " up " << trigger.via
               << ", but binds no '!ent.ref<" << trigger.component << ", up "
               << trigger.via << ">': the ancestor is the one such a ref "
               << "leads to";
    }
    if (trigger.logCapacity && *trigger.logCapacity < 0)
      return emitOpError("gives the event log of ")
             << trigger.component << " a negative capacity";
    if (!system.canRead(trigger.component))
      return emitOpError("reacts to ")
             << trigger.component << " but system @" << system.getSymName()
             << " does not declare it in 'reads' or 'writes'";
    // An entity that lost the component cannot match a query binding it.
    if (trigger.kind == Trigger::Removed && !trigger.via &&
        llvm::is_contained(getRequired(), trigger.component))
      return emitOpError("reacts to removed ")
             << trigger.component
             << " but requires it; an entity that lost it never matches";
  }
  SmallVector<FlatSymbolRefAttr> filters = getWithout();
  if (auto with = (*this)->getAttr(kWithAttr))
    llvm::append_range(filters, *componentList(with));
  for (auto &group : getAnyGroups())
    llvm::append_range(filters, group);
  for (FlatSymbolRefAttr component : filters) {
    if (!lookupComponent(symbolTable, *this, component))
      return emitOpError("filters by unknown component ") << component;
    if (!system.canRead(component))
      return emitOpError("filters by ")
             << component << " but system @" << system.getSymName()
             << " does not declare it in 'reads' or 'writes'";
  }
  // `up @R` and `cascade @R` follow a tree.
  auto verifyTree = [&](FlatSymbolRefAttr name,
                        StringRef what) -> LogicalResult {
    auto relation =
        symbolTable.lookupNearestSymbolFrom<RelationOp>(*this, name);
    if (!relation)
      return emitOpError(what) << " unknown relation " << name;
    if (!relation.getTree())
      return emitOpError(what)
             << " " << name << ", which is not declared a 'tree'";
    if (!system.canRead(name))
      return emitOpError(what)
             << " " << name << " but system @" << system.getSymName()
             << " does not declare it in 'reads' or 'writes'";
    return success();
  };
  if (FlatSymbolRefAttr cascade = getCascade())
    if (failed(verifyTree(cascade, "cascades along")))
      return failure();
  for (BlockArgument arg : getBody().getArguments()) {
    auto refType = cast<RefType>(arg.getType());
    FlatSymbolRefAttr component = refType.getComponent();
    if (refType.isUp() &&
        failed(verifyTree(refType.getVia(),
                          refType.getIsBefore() ? "binds before" : "binds up")))
      return failure();
    if (refType.getIsBefore() &&
        !symbolTable
             .lookupNearestSymbolFrom<RelationOp>(*this, refType.getVia())
             .isOrdered())
      return emitOpError("binds before ")
             << refType.getVia()
             << ", whose entities are in no order ('tree ordered by')";
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

/// Check the rules shared by ent.add and ent.remove and return the
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
    return op->emitOpError("must be inside an 'ent.query': it changes the "
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

// `@Archetype(values)`, `(@A, @B)(values)` or `(@A, @B) into @A_B (values)`,
// then `: types` when there are values.
ParseResult SpawnOp::parse(OpAsmParser &parser, OperationState &result) {
  Builder &b = parser.getBuilder();
  FlatSymbolRefAttr archetype;
  if (succeeded(parser.parseOptionalLParen())) {
    SmallVector<Attribute> components;
    if (parser.parseCommaSeparatedList([&]() -> ParseResult {
          FlatSymbolRefAttr component;
          if (parser.parseAttribute(component))
            return failure();
          components.push_back(component);
          return success();
        }) ||
        parser.parseRParen())
      return failure();
    result.addAttribute(getComponentsAttrName(result.name),
                        b.getArrayAttr(components));
    if (succeeded(parser.parseOptionalKeyword("into")) &&
        parser.parseAttribute(archetype))
      return failure();
  } else if (parser.parseAttribute(archetype)) {
    return failure();
  }
  if (archetype)
    result.addAttribute(getArchetypeAttrName(result.name), archetype);
  SmallVector<OpAsmParser::UnresolvedOperand> values;
  SmallVector<Type> types;
  if (parser.parseOperandList(values, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  if (succeeded(parser.parseOptionalColon()) && parser.parseTypeList(types))
    return failure();
  if (parser.resolveOperands(values, types, parser.getCurrentLocation(),
                             result.operands))
    return failure();
  result.addTypes(EntityType::get(parser.getContext()));
  return success();
}

void SpawnOp::print(OpAsmPrinter &p) {
  p << " ";
  if (ArrayAttr components = getComponentsAttr()) {
    p << "(";
    llvm::interleaveComma(components, p);
    p << ")";
    if (FlatSymbolRefAttr archetype = getArchetypeAttr())
      p << " into " << archetype << " ";
  } else {
    p << getArchetypeAttr();
  }
  p << "(" << getValues() << ")";
  p.printOptionalAttrDict((*this)->getAttrs(),
                          {getArchetypeAttrName(), getComponentsAttrName()});
  if (!getValues().empty())
    p << " : " << getValues().getTypes();
}

bool SpawnOp::startsWith(FlatSymbolRefAttr component) {
  if (ArrayAttr components = getComponentsAttr())
    return llvm::is_contained(components, component);
  auto archetype = SymbolTable::lookupNearestSymbolFrom<ArchetypeOp>(
      *this, getArchetypeAttr());
  return archetype && archetype.contains(component) &&
         !archetype.isOptional(component);
}

LogicalResult SpawnOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ent.system'");
  if (!getArchetypeAttr() && !getComponentsAttr())
    return emitOpError("names neither an archetype nor components");
  if (ArrayAttr components = getComponentsAttr()) {
    if (components.empty())
      return emitOpError("spawns an entity without components");
    llvm::SmallPtrSet<Attribute, 8> seen;
    for (Attribute component : components)
      if (!seen.insert(component).second)
        return emitOpError("lists component ")
               << component << " more than once";
  }
  return success();
}

LogicalResult SpawnOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system = (*this)->getParentOfType<SystemOp>();
  ArchetypeOp archetype;
  if (FlatSymbolRefAttr name = getArchetypeAttr()) {
    archetype = symbolTable.lookupNearestSymbolFrom<ArchetypeOp>(*this, name);
    if (!archetype)
      return emitOpError("references unknown archetype ") << name;
  }

  // The components whose fields the values initialise, in order.
  SmallVector<FlatSymbolRefAttr> initialised;
  if (ArrayAttr components = getComponentsAttr()) {
    for (Attribute attr : components) {
      auto component = cast<FlatSymbolRefAttr>(attr);
      if (!lookupComponent(symbolTable, *this, component))
        return emitOpError("spawns unknown component ") << component;
      if (!system.canWrite(component))
        return emitOpError("spawns ")
               << component << " but system @" << system.getSymName()
               << " does not declare it in 'writes'";
      initialised.push_back(component);
    }
    // An archetype recorded by inference holds every listed component and
    // requires no other.
    if (archetype) {
      for (FlatSymbolRefAttr component : initialised)
        if (!archetype.contains(component))
          return emitOpError("spawns into ")
                 << getArchetypeAttr() << ", which does not hold "
                 << component;
      for (Attribute attr : archetype.getComponents()) {
        auto component = cast<FlatSymbolRefAttr>(attr);
        if (!archetype.isOptional(component) &&
            !llvm::is_contained(initialised, component))
          return emitOpError("spawns into ")
                 << getArchetypeAttr() << " without its required component "
                 << component;
      }
    }
  } else {
    if (!system.canWrite(getArchetypeAttr()))
      return emitOpError("spawns into ")
             << getArchetypeAttr() << " but system @" << system.getSymName()
             << " does not declare it in 'writes'";
    for (Attribute attr : archetype.getComponents())
      if (!archetype.isOptional(cast<FlatSymbolRefAttr>(attr)))
        initialised.push_back(cast<FlatSymbolRefAttr>(attr));
  }

  // Every field of every initialised component, in order.
  SmallVector<std::pair<std::string, Type>> fields;
  for (FlatSymbolRefAttr ref : initialised) {
    ComponentOp component = lookupComponent(symbolTable, *this, ref);
    for (auto [name, type] :
         llvm::zip(component.getFieldNames(), component.getFieldTypes()))
      fields.push_back({(ref.getValue() + "." +
                         cast<StringAttr>(name).getValue())
                            .str(),
                        cast<TypeAttr>(type).getValue()});
  }
  if (fields.size() != getValues().size()) {
    InFlightDiagnostic diag = emitOpError("initialises ")
                              << getValues().size() << " fields, but ";
    if (getComponentsAttr())
      diag << "its components have " << fields.size();
    else
      diag << "the non-optional components of " << getArchetypeAttr()
           << " have " << fields.size();
    return diag;
  }
  for (auto [index, value, field] : llvm::enumerate(getValues(), fields))
    if (value.getType() != field.second)
      return emitOpError("value #")
             << index << " has type " << value.getType() << ", but field "
             << field.first << " has type " << field.second;
  return success();
}

LogicalResult LookupOp::verify() {
  if (!(*this)->getParentOfType<SystemOp>())
    return emitOpError("must be inside an 'ent.system'");
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

LogicalResult CombineOp::verify() {
  RefType ref = getRef().getType();
  auto arg = dyn_cast<BlockArgument>(getRef());
  auto query = arg ? dyn_cast<QueryOp>(arg.getOwner()->getParentOp())
                   : QueryOp();
  if (!ref.isUp() || ref.getIsBefore() || !query)
    return emitOpError("combines through ")
           << ref << "; only a ref up a relation, bound by the query, leads "
           << "to another entity";
  if (!ref.getIsMutable())
    return emitOpError("requires a mutable reference, got ") << ref;
  if (getRule() != "add" && getRule() != "min" && getRule() != "max")
    return emitOpError("has unknown rule '")
           << getRule() << "'; expected 'add', 'min' or 'max'";
  // Without an order, the values would have to wait for the query's end.
  if (query.getCascade() != ref.getVia())
    return emitOpError("combines into an ancestor up ")
           << ref.getVia() << " in a query that does not cascade along it, "
           << "which is not supported yet";
  if ((*this)->getParentOfType<EdgesOp>())
    return emitOpError("inside 'ent.edges' is not supported yet");

  // What this depth sends must not be seen by this depth.
  Operation *reader = nullptr;
  CombineOp other;
  query.walk([&](Operation *op) {
    if (auto get = dyn_cast<GetOp>(op)) {
      RefType read = get.getRef().getType();
      if (read.isUp() && read.getComponent() == ref.getComponent() &&
          get.getField() == getField())
        reader = op;
    } else if (auto lookup = dyn_cast<LookupOp>(op)) {
      if (lookup.getComponentAttr() == ref.getComponent() &&
          lookup.getField() == getField())
        reader = op;
    } else if (auto combine = dyn_cast<CombineOp>(op)) {
      if (!other && combine.getRef().getType().getComponent() ==
                        ref.getComponent() &&
          combine.getField() == getField() && combine.getRule() != getRule())
        other = combine;
    }
  });
  if (reader) {
    InFlightDiagnostic diag =
        emitOpError("combines into ")
        << ref.getComponent() << " \"" << getField()
        << "\" of an ancestor in a query that reads it of other entities; "
           "which values those see would depend on the order";
    diag.attachNote(reader->getLoc()) << "read here";
    return diag;
  }
  if (other) {
    InFlightDiagnostic diag =
        emitOpError("combines ")
        << ref.getComponent() << " \"" << getField() << "\" with '"
        << getRule() << "', but the query also combines it with '"
        << other.getRule() << "'; the result would depend on the order";
    diag.attachNote(other.getLoc()) << "other rule here";
    return diag;
  }
  return success();
}

LogicalResult CombineOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveField(symbolTable, *this, getRef().getType(), getField());
  if (failed(fieldType))
    return failure();
  if (*fieldType != getValue().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  if ((!isa<FloatType>(*fieldType) && !fieldType->isIntOrIndex()) ||
      fieldType->isInteger(1))
    return emitOpError("cannot combine field '")
           << getField() << "' of type " << *fieldType
           << "; only integers and floats can";
  auto system = (*this)->getParentOfType<SystemOp>();
  FlatSymbolRefAttr component = getRef().getType().getComponent();
  if (!system.canWrite(component))
    return emitOpError("combines into ")
           << component << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";
  return success();
}

/// Checks shared by ent.apply and ent.accumulate: inside a query, at most
/// once per entity (not inside a loop there: the buffer has one slot per
/// row), and a known rule.
static LogicalResult verifyCombining(Operation *op, StringRef rule) {
  auto query = op->getParentOfType<QueryOp>();
  if (!query)
    return op->emitOpError("must be inside an 'ent.query': its values are "
                           "combined when the query ends");
  for (Operation *parent = op->getParentOp(); parent != query;
       parent = parent->getParentOp())
    if (isa<LoopLikeOpInterface>(parent))
      return op->emitOpError("must not be inside a loop ('")
             << parent->getName() << "'): it may run at most once per entity";
  // An apply in ent.edges runs once per edge, into a slot per edge; an
  // accumulate there has no such buffer yet.
  if (isa<AccumulateOp>(op) && op->getParentOfType<EdgesOp>())
    return op->emitOpError("inside 'ent.edges' is not supported yet");
  if (rule != "add" && rule != "min" && rule != "max")
    return op->emitOpError("has unknown rule '")
           << rule << "'; expected 'add', 'min' or 'max'";
  return success();
}

/// Whether `type` can be combined: an integer other than i1, or a float.
static bool isCombinable(Type type) {
  return (isa<FloatType>(type) || type.isIntOrIndex()) && !type.isInteger(1);
}

LogicalResult ApplyOp::verify() { return verifyCombining(*this, getRule()); }

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

//===----------------------------------------------------------------------===//
// EdgesOp / ConnectOp / DisconnectOp
//===----------------------------------------------------------------------===//

// ent.edges @R out|in (%e: !ent.ref<@R[, mut]>, %other: !ent.entity) { }
ParseResult EdgesOp::parse(OpAsmParser &parser, OperationState &result) {
  FlatSymbolRefAttr relation;
  StringRef direction;
  if (parser.parseAttribute(relation) || parser.parseKeyword(&direction))
    return failure();
  Builder &b = parser.getBuilder();
  result.addAttribute(getRelationAttrName(result.name), relation);
  result.addAttribute(getDirectionAttrName(result.name),
                      b.getStringAttr(direction));
  return parseArgsAndBody<EdgesOp>(parser, result);
}

void EdgesOp::print(OpAsmPrinter &p) {
  p << " " << getRelationAttr() << " " << getDirection() << " ";
  printArgs(p, getBody());
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(), {getRelationAttrName(), getDirectionAttrName()});
  printBody(p, getBody());
}

LogicalResult EdgesOp::verify() {
  if (getDirection() != "out" && getDirection() != "in")
    return emitOpError("direction must be 'out' or 'in', got '")
           << getDirection() << "'";
  if (!(*this)->getParentOfType<QueryOp>())
    return emitOpError("must be inside an 'ent.query': it visits the edges "
                       "of the entity the query visits");
  if ((*this)->getParentOfType<EdgesOp>())
    return emitOpError("cannot be nested in another 'ent.edges'");
  for (Operation *parent = (*this)->getParentOp(); !isa<QueryOp>(parent);
       parent = parent->getParentOp())
    if (isa<LoopLikeOpInterface>(parent))
      return emitOpError("must not be inside a loop ('")
             << parent->getName() << "')";
  Block &body = getBody().front();
  auto ref = body.getNumArguments() == 2
                 ? dyn_cast<RefType>(body.getArgument(0).getType())
                 : RefType();
  if (!ref || !isa<EntityType>(body.getArgument(1).getType()))
    return emitOpError("must take an !ent.ref to the relation and the "
                       "!ent.entity at the other end");
  if (ref.getComponent() != getRelationAttr())
    return emitOpError("ref names ")
           << ref.getComponent() << ", but the loop visits "
           << getRelationAttr();
  for (Operation *user : body.getArgument(0).getUsers())
    if (!isa<GetOp, SetOp>(user))
      return user->emitOpError("uses the edge reference; it may only be "
                               "used by 'ent.get' and 'ent.set'");
  return success();
}

LogicalResult EdgesOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  if (!symbolTable.lookupNearestSymbolFrom<RelationOp>(*this,
                                                       getRelationAttr()))
    return emitOpError("visits unknown relation ") << getRelationAttr();
  auto system = (*this)->getParentOfType<SystemOp>();
  bool mut = cast<RefType>(getBody().getArgument(0).getType()).getIsMutable();
  if (mut && !system.canWrite(getRelationAttr()))
    return emitOpError("writes the edges of ")
           << getRelationAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";
  if (!system.canRead(getRelationAttr()))
    return emitOpError("visits the edges of ")
           << getRelationAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'reads' or 'writes'";
  // Both ways in one query: an edge would be visited by its source and its
  // target, from different entities' iterations.
  EdgesOp other;
  (*this)->getParentOfType<QueryOp>().walk([&](EdgesOp edges) {
    if (!other && edges.getRelationAttr() == getRelationAttr() &&
        edges.getDirection() != getDirection())
      other = edges;
  });
  if (other) {
    bool otherMut =
        cast<RefType>(other.getBody().getArgument(0).getType()).getIsMutable();
    if (mut || otherMut) {
      InFlightDiagnostic diag =
          emitOpError("visits ")
          << getRelationAttr()
          << " both ways in one query and one loop writes the edges; an "
             "edge would be visited from both its ends";
      diag.attachNote(other.getLoc()) << "other loop here";
      return diag;
    }
  }
  return success();
}

LogicalResult ConnectOp::verify() {
  if (auto query = (*this)->getParentOfType<QueryOp>()) {
    if ((*this)->getParentOfType<EdgesOp>())
      return emitOpError("inside 'ent.edges' is not supported yet");
    for (Operation *parent = (*this)->getParentOp(); parent != query;
         parent = parent->getParentOp())
      if (isa<LoopLikeOpInterface>(parent))
        return emitOpError("must not be inside a loop ('")
               << parent->getName()
               << "') in a query: it may run at most once per entity";
  } else if (!(*this)->getParentOfType<SystemOp>()) {
    return emitOpError("must be inside an 'ent.system'");
  }
  return success();
}

LogicalResult ConnectOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto relation =
      symbolTable.lookupNearestSymbolFrom<RelationOp>(*this, getRelationAttr());
  if (!relation)
    return emitOpError("connects unknown relation ") << getRelationAttr();
  ArrayAttr fieldTypes = relation.getFieldTypes();
  if (fieldTypes.size() != getValues().size())
    return emitOpError("initialises ")
           << getValues().size() << " fields, but " << getRelationAttr()
           << " has " << fieldTypes.size();
  for (auto [index, value, typeAttr] :
       llvm::enumerate(getValues(), fieldTypes))
    if (value.getType() != cast<TypeAttr>(typeAttr).getValue())
      return emitOpError("value #")
             << index << " has type " << value.getType() << ", but field '"
             << cast<StringAttr>(relation.getFieldNames()[index]).getValue()
             << "' has type " << cast<TypeAttr>(typeAttr).getValue();
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canWrite(getRelationAttr()))
    return emitOpError("connects ")
           << getRelationAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'writes'";
  return success();
}

LogicalResult DisconnectOp::verify() {
  auto edges = (*this)->getParentOfType<EdgesOp>();
  if (!edges)
    return emitOpError("must be inside an 'ent.edges': it removes the edge "
                       "the loop visits");
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canWrite(edges.getRelationAttr()))
    return emitOpError("disconnects ")
           << edges.getRelationAttr() << " but system @"
           << system.getSymName() << " does not declare it in 'writes'";
  return success();
}

LogicalResult DespawnOp::verify() { return verifyInsideQuery(*this); }

LogicalResult EntityOp::verify() { return verifyInsideQuery(*this); }

LogicalResult HasOp::verify() { return verifyInsideQuery(*this); }

LogicalResult HasOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  if (!lookupComponent(symbolTable, *this, getComponentAttr()))
    return emitOpError("references unknown component ") << getComponentAttr();
  auto system = (*this)->getParentOfType<SystemOp>();
  if (!system.canRead(getComponentAttr()))
    return emitOpError("tests for ")
           << getComponentAttr() << " but system @" << system.getSymName()
           << " does not declare it in 'reads' or 'writes'";
  return success();
}

LogicalResult DespawnOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto system = (*this)->getParentOfType<SystemOp>();
  auto query = (*this)->getParentOfType<QueryOp>();
  for (ArchetypeOp archetype : getMatchedArchetypes(query)) {
    // An inferred archetype has no name a contract could list: the query's
    // required components stand for it.
    if (archetype.getInferred()) {
      for (FlatSymbolRefAttr component : query.getRequired()) {
        if (!system.canWrite(component))
          return emitOpError("despawns entities with ")
                 << component << " but system @" << system.getSymName()
                 << " does not declare it in 'writes'";
      }
      continue;
    }
    auto ref = FlatSymbolRefAttr::get(archetype.getSymNameAttr());
    if (!system.canWrite(ref))
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
  if (!system) // a condition, which has no contract
    return fieldType;
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
  Region *region = (*this)->getParentRegion();
  auto schedule = dyn_cast<ScheduleOp>(region->getParentOp());
  if (!(*this)->getParentOfType<SystemOp>() &&
      !(*this)->getParentOfType<RunOp>() &&
      !(*this)->getParentOfType<MainOp>() &&
      !(schedule && region == &schedule.getCondition()))
    return emitOpError("must be inside an 'ent.system', a condition or "
                       "'ent.main'");
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
    return emitOpError("must be inside an 'ent.system'");
  if ((*this)->getParentOfType<QueryOp>())
    return emitOpError("cannot write a resource inside 'ent.query': every "
                       "entity would write the same field, which makes the "
                       "entities depend on each other; write it at system "
                       "level, or combine values with 'ent.accumulate'");
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

LogicalResult AccumulateOp::verify() {
  return verifyCombining(*this, getRule());
}

LogicalResult
AccumulateOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  FailureOr<Type> fieldType =
      resolveResourceField(symbolTable, *this, getResourceAttr(), getField(),
                           /*write=*/true);
  if (failed(fieldType))
    return failure();
  if (*fieldType != getValue().getType())
    return emitOpError("value type ")
           << getValue().getType() << " does not match field '" << getField()
           << "' of type " << *fieldType;
  if (!isCombinable(*fieldType))
    return emitOpError("cannot combine field '")
           << getField() << "' of type " << *fieldType
           << "; only integers (not i1) and floats can";
  // One rule per field and query: mixed rules would not commute.
  AccumulateOp other;
  (*this)->getParentOfType<QueryOp>().walk([&](AccumulateOp accumulate) {
    if (!other && accumulate.getResourceAttr() == getResourceAttr() &&
        accumulate.getField() == getField() &&
        accumulate.getRule() != getRule())
      other = accumulate;
  });
  if (other) {
    InFlightDiagnostic diag =
        emitOpError("combines ")
        << getResourceAttr() << " \"" << getField() << "\" with '"
        << getRule() << "', but the query also combines it with '"
        << other.getRule() << "'; the result would depend on the order";
    diag.attachNote(other.getLoc()) << "other rule here";
    return diag;
  }
  return success();
}

LogicalResult RunOp::verify() {
  Region &condition = getCondition();
  if (!condition.empty() && condition.front().getNumArguments() != 0)
    return emitOpError("condition takes no arguments; it uses the "
                       "schedule's parameters directly");
  return verifyCondition(*this, condition);
}

LogicalResult RunOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  Operation *callee =
      symbolTable.lookupNearestSymbolFrom(*this, getSystemAttr());
  SmallVector<Type> params;
  if (auto system = dyn_cast_or_null<SystemOp>(callee))
    llvm::append_range(params, system.getBody().getArgumentTypes());
  else if (auto external = dyn_cast_or_null<ExternOp>(callee))
    llvm::append_range(params, external.getParams().getAsValueRange<TypeAttr>());
  else
    return emitOpError("references unknown system ") << getSystemAttr();

  if (TypeRange(params) != getArgs().getTypes())
    return emitOpError("argument types (")
           << getArgs().getTypes() << ") do not match the parameters ("
           << params << ") of system " << getSystemAttr();
  return success();
}
