#include "Ecs/EcsDialect.h"
#include "Ecs/Passes.h"

#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  mlir::registerAllPasses();
  mlir::ecs::registerEcsPasses();

  mlir::DialectRegistry registry;
  registry.insert<mlir::ecs::EcsDialect>();
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "ECS optimizer driver\n", registry));
}
