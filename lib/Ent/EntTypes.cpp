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
  bool isMutable = false;
  while (succeeded(parser.parseOptionalComma())) {
    if (succeeded(parser.parseOptionalKeyword("up"))) {
      if (parser.parseAttribute(via))
        return {};
    } else if (parser.parseKeyword("mut")) {
      return {};
    } else {
      isMutable = true;
    }
  }
  if (parser.parseGreater())
    return {};
  return RefType::get(parser.getContext(), component, isMutable, via);
}

void RefType::print(AsmPrinter &printer) const {
  printer << "<" << getComponent();
  if (getIsMutable())
    printer << ", mut";
  if (getVia())
    printer << ", up " << getVia();
  printer << ">";
}
