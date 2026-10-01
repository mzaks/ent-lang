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
  // `capacity N`, mandatory for relations.
  if constexpr (std::is_same_v<OpTy, RelationOp>) {
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
    p << " capacity " << op.getCapacity();
    elided.push_back(op.getCapacityAttrName());
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

ParseResult RelationOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseRecord<RelationOp>(parser, result);
}
void RelationOp::print(OpAsmPrinter &p) { printRecord(*this, p); }
LogicalResult RelationOp::verify() {
  return verifyRecord(*this, getFieldNames(), getFieldTypes());
}
Type RelationOp::getFieldType(StringRef name) {
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

LogicalResult SystemOp::verify() {
  if (failed(verifyNoRefParams(*this, getBody())))
    return failure();

  llvm::SmallPtrSet<Attribute, 8> seen;
  for (auto [listName, list] :
       {std::pair<StringRef, ArrayAttr>{"reads", getReadsAttr()},
        std::pair<StringRef, ArrayAttr>{"writes", getWritesAttr()}}) {
    if (!list)
      continue;
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
  for (ArrayAttr list : {getReadsAttr(), getWritesAttr()}) {
    if (!list)
      continue;
    for (Attribute attr : list) {
      auto ref = cast<FlatSymbolRefAttr>(attr);
      Operation *target = symbolTable.lookupNearestSymbolFrom(*this, ref);
      if (!isa_and_nonnull<ComponentOp, ResourceOp, ArchetypeOp, RelationOp>(
              target))
        return emitOpError("declares access to unknown component, resource, "
                           "relation "
                           "or archetype ")
               << ref;
      if (isa<ArchetypeOp>(target) && list == getReadsAttr())
        return emitOpError("lists archetype ")
               << ref
               << " in 'reads'; archetypes are declared in 'writes', by "
                  "systems that spawn or despawn their entities";
    }
  }
  return success();
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
  if (auto with = (*this)->getAttrOfType<ArrayAttr>(kWithAttr))
    p << " with " << with;
  if (auto without = (*this)->getAttrOfType<ArrayAttr>(kWithoutAttr))
    p << " without " << without;
  if (auto groups = (*this)->getAttrOfType<ArrayAttr>(kAnyAttr))
    for (Attribute group : groups)
      p << " any " << group;
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
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(), {kTriggersAttr, kWithAttr, kWithoutAttr, kAnyAttr});
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
    required.push_back(cast<RefType>(type).getComponent());
  if (Attribute with = (*this)->getAttr(kWithAttr))
    if (auto list = componentList(with); succeeded(list))
      required.append(list->begin(), list->end());
  return required;
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

  llvm::SmallPtrSet<Attribute, 8> seen;
  for (BlockArgument arg : body.getArguments()) {
    auto refType = dyn_cast<RefType>(arg.getType());
    if (!refType)
      return emitOpError("argument #")
             << arg.getArgNumber() << " must be an !ent.ref, got "
             << arg.getType();
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
      !(schedule && region == &schedule.getCondition()))
    return emitOpError("must be inside an 'ent.system' or a condition");
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
