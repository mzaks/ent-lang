#include "Ent/EntTypes.h"
#include "Ent/EntDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::ent;

#define GET_TYPEDEF_CLASSES
#include "Ent/EntOpsTypes.cpp.inc"

// !ent.enum<@Name>
Type EnumType::parse(AsmParser &parser) {
  FlatSymbolRefAttr name;
  if (parser.parseLess() || parser.parseAttribute(name) ||
      parser.parseGreater())
    return {};
  return EnumType::get(parser.getContext(), name);
}

void EnumType::print(AsmPrinter &printer) const {
  printer << "<" << getName() << ">";
}

void EntDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "Ent/EntOpsTypes.cpp.inc"
      >();
}

// !ent.ref<@Component>, !ent.ref<@Component, mut> or
// !ent.ref<@Component, up @Relation>
Type RefType::parse(AsmParser &parser) {
  FlatSymbolRefAttr component, via;
  if (parser.parseLess() || parser.parseAttribute(component))
    return {};
  bool isMutable = false, isBefore = false, isOptional = false;
  bool isDirect = false, isAfter = false;
  unsigned hops = 1;
  ArrayAttr path;
  while (succeeded(parser.parseOptionalComma())) {
    if (succeeded(parser.parseOptionalKeyword("up"))) {
      if (parser.parseAttribute(via))
        return {};
    } else if (succeeded(parser.parseOptionalKeyword("parent"))) {
      if (parser.parseAttribute(via))
        return {};
      isDirect = true;
    } else if (succeeded(parser.parseOptionalKeyword("before"))) {
      if (parser.parseAttribute(via))
        return {};
      isBefore = true;
    } else if (succeeded(parser.parseOptionalKeyword("after"))) {
      if (parser.parseAttribute(via))
        return {};
      isAfter = true;
    } else if (succeeded(parser.parseOptionalKeyword("hops"))) {
      if (parser.parseInteger(hops))
        return {};
    } else if (succeeded(parser.parseOptionalKeyword("path"))) {
      // [["parent", @R], ["up", @R, @C, ...], ...]; the first step's tree
      // is the ref's.
      if (parser.parseAttribute(path))
        return {};
      bool wellFormed = !path.empty();
      for (Attribute attr : path) {
        auto step = dyn_cast<ArrayAttr>(attr);
        wellFormed &= step && step.size() >= 2 && isa<StringAttr>(step[0]) &&
                      llvm::all_of(step.getValue().drop_front(),
                                   [](Attribute part) {
                                     return isa<FlatSymbolRefAttr>(part);
                                   });
      }
      if (!wellFormed) {
        parser.emitError(parser.getCurrentLocation(),
                         "a path is steps, each a kind and a relation: "
                         "[[\"parent\", @R], [\"up\", @R, @C]]");
        return {};
      }
      via = cast<FlatSymbolRefAttr>(cast<ArrayAttr>(path[0])[1]);
      isDirect = true;
    } else if (succeeded(parser.parseOptionalKeyword("optional"))) {
      isOptional = true;
    } else if (parser.parseKeyword("mut")) {
      return {};
    } else {
      isMutable = true;
    }
  }
  if (parser.parseGreater())
    return {};
  return RefType::get(parser.getContext(), component, isMutable, via,
                      isBefore, isOptional, isDirect, isAfter, hops, path);
}

void RefType::print(AsmPrinter &printer) const {
  printer << "<" << getComponent();
  if (getIsMutable())
    printer << ", mut";
  if (hasPath())
    printer << ", path " << getPath();
  else if (getVia())
    printer << (getIsBefore()   ? ", before "
                : getIsAfter()  ? ", after "
                : getIsDirect() ? ", parent "
                                : ", up ")
            << getVia();
  if (getHops() != 1)
    printer << ", hops " << getHops();
  if (getIsOptional())
    printer << ", optional";
  printer << ">";
}
