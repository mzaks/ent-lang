#include "Ecs/EcsTypes.h"
#include "Ecs/EcsDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::ecs;

#define GET_TYPEDEF_CLASSES
#include "Ecs/EcsOpsTypes.cpp.inc"

void EcsDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "Ecs/EcsOpsTypes.cpp.inc"
      >();
}

// !ecs.ref<@Component> or !ecs.ref<@Component, mut>
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
