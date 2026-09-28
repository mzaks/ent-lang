#include "Ecs/EcsOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "llvm/ADT/STLExtras.h"

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
