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

// !ent.ref<@Component> or !ent.ref<@Component, mut>
Type RefType::parse(AsmParser &parser) {
  FlatSymbolRefAttr component;
  if (parser.parseLess() || parser.parseAttribute(component))
    return {};
  bool isMutable = false;
  if (succeeded(parser.parseOptionalComma())) {
    if (parser.parseKeyword("mut"))
      return {};
    isMutable = true;
  }
  if (parser.parseGreater())
    return {};
  return RefType::get(parser.getContext(), component, isMutable);
}

void RefType::print(AsmPrinter &printer) const {
  printer << "<" << getComponent();
  if (getIsMutable())
    printer << ", mut";
  printer << ">";
}
