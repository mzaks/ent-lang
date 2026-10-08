#include "Ent/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTLOWERVIEWS
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// The world is one block of bytes, and a column is read through a view
/// of it (`memref.view`): a memref of its own, which becomes a record of
/// five values when it is lowered, made and taken apart again for every
/// column a function touches. Most of what a big program is lowered to is
/// that. A load or a store through such a view only needs an address:
/// the block's, so many bytes on, so many elements on.
struct EntLowerViews : public mlir::ent::impl::EntLowerViewsBase<EntLowerViews> {
  void runOnOperation() override {
    func::FuncOp func = getOperation();
    if (func.isExternal())
      return;
    OpBuilder builder(func.getContext());
    Type pointer = LLVM::LLVMPointerType::get(func.getContext());
    Type i64 = builder.getI64Type();
    Type i8 = builder.getI8Type();
    // The address of each block that is viewed, made where the block is.
    llvm::DenseMap<Value, Value> addresses;
    auto addressOf = [&](Value block) -> Value {
      Value &address = addresses[block];
      if (address)
        return address;
      Location loc = block.getLoc();
      if (auto result = dyn_cast<OpResult>(block))
        builder.setInsertionPointAfter(result.getOwner());
      else
        builder.setInsertionPointToStart(
            cast<BlockArgument>(block).getOwner());
      Value index =
          memref::ExtractAlignedPointerAsIndexOp::create(builder, loc, block);
      Value bits = arith::IndexCastOp::create(builder, loc, i64, index);
      address = LLVM::IntToPtrOp::create(builder, loc, pointer, bits);
      return address;
    };
    auto asI64 = [&](Location loc, Value index) -> Value {
      if (index.getType() == i64)
        return index;
      return arith::IndexCastOp::create(builder, loc, i64, index);
    };

    SmallVector<memref::ViewOp> views;
    func.walk([&](memref::ViewOp view) { views.push_back(view); });
    for (memref::ViewOp view : views) {
      auto type = cast<MemRefType>(view.getType());
      Type element = type.getElementType();
      // (A column of numbers that are stored as they are computed with.)
      if (type.getRank() != 1 || !isa<IntegerType, FloatType>(element) ||
          cast<MemRefType>(view.getSource().getType()).getElementType() != i8)
        continue;
      SmallVector<Operation *> users;
      for (OpOperand &use : view->getUses()) {
        Operation *user = use.getOwner();
        if (auto load = dyn_cast<memref::LoadOp>(user)) {
          if (load.getIndices().size() == 1)
            users.push_back(user);
        } else if (auto store = dyn_cast<memref::StoreOp>(user)) {
          // (Not where the view is what is stored.)
          if (store.getIndices().size() == 1 &&
              use.getOperandNumber() == 1)
            users.push_back(user);
        }
      }
      if (users.empty())
        continue;
      Location loc = view.getLoc();
      Value block = addressOf(view.getSource());
      builder.setInsertionPoint(view);
      Value column = LLVM::GEPOp::create(
          builder, loc, pointer, i8, block,
          ArrayRef<LLVM::GEPArg>{asI64(loc, view.getByteShift())});
      for (Operation *user : users) {
        builder.setInsertionPoint(user);
        Location at = user->getLoc();
        if (auto load = dyn_cast<memref::LoadOp>(user)) {
          Value place = LLVM::GEPOp::create(
              builder, at, pointer, element, column,
              ArrayRef<LLVM::GEPArg>{asI64(at, load.getIndices()[0])});
          Value value = LLVM::LoadOp::create(builder, at, element, place);
          load.getResult().replaceAllUsesWith(value);
          load.erase();
        } else {
          auto store = cast<memref::StoreOp>(user);
          Value place = LLVM::GEPOp::create(
              builder, at, pointer, element, column,
              ArrayRef<LLVM::GEPArg>{asI64(at, store.getIndices()[0])});
          LLVM::StoreOp::create(builder, at, store.getValueToStore(), place);
          store.erase();
        }
      }
      if (view->use_empty())
        view.erase();
    }
  }
};

} // namespace
