#include "Ecs/EcsOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace mlir;
using namespace mlir::ecs;

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

//===----------------------------------------------------------------------===//
// ComponentOp
//===----------------------------------------------------------------------===//

// ecs.component @Name (field: type, ...)
ParseResult ComponentOp::parse(OpAsmParser &parser, OperationState &result) {
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
  result.addAttribute(getFieldNamesAttrName(result.name),
                      b.getArrayAttr(names));
  result.addAttribute(getFieldTypesAttrName(result.name),
                      b.getArrayAttr(types));
  return parser.parseOptionalAttrDict(result.attributes);
}

void ComponentOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printSymbolName(getSymName());
  p << " (";
  llvm::interleaveComma(
      llvm::zip(getFieldNames(), getFieldTypes()), p, [&](auto field) {
        p.printKeywordOrString(cast<StringAttr>(std::get<0>(field)).getValue());
        p << ": " << cast<TypeAttr>(std::get<1>(field)).getValue();
      });
  p << ")";
  p.printOptionalAttrDict((*this)->getAttrs(),
                          {getSymNameAttrName(), getFieldNamesAttrName(),
                           getFieldTypesAttrName()});
}

LogicalResult ComponentOp::verify() {
  if (getFieldNames().size() != getFieldTypes().size())
    return emitOpError("has ") << getFieldNames().size() << " field names but "
                               << getFieldTypes().size() << " field types";

  llvm::StringSet<> seen;
  for (auto [nameAttr, typeAttr] :
       llvm::zip(getFieldNames(), getFieldTypes())) {
    StringRef name = cast<StringAttr>(nameAttr).getValue();
    if (!seen.insert(name).second)
      return emitOpError("has duplicate field '") << name << "'";
    // Scalars only for now: layout passes split components into one column
    // per field, which needs every field to be a plain value.
    Type type = cast<TypeAttr>(typeAttr).getValue();
    if (!isa<IntegerType, FloatType, IndexType>(type))
      return emitOpError("field '")
             << name << "' has type " << type
             << "; only integer, float and index fields are supported";
  }
  return success();
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
        return emitOpError("lists component ")
               << attr
               << " more than once; 'writes' already implies read access";
    }
  }
  return success();
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
// QueryOp
//===----------------------------------------------------------------------===//

ParseResult QueryOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseArgsAndBody<QueryOp>(parser, result);
}

void QueryOp::print(OpAsmPrinter &p) {
  p << " ";
  printArgs(p, getBody());
  p.printOptionalAttrDictWithKeyword((*this)->getAttrs());
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
  }
  return success();
}

//===----------------------------------------------------------------------===//
// GetOp / SetOp
//===----------------------------------------------------------------------===//

LogicalResult SetOp::verify() {
  if (!getRef().getType().getIsMutable())
    return emitOpError("requires a mutable reference, got ")
           << getRef().getType();
  return success();
}
