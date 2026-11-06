#include "Ent/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

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
/// The bytes of the world a view is of, where they are known: a view of
/// the world itself (a function's argument of the world's type) at a
/// constant offset, of a number of elements known from its type.
static std::optional<std::pair<int64_t, int64_t>>
worldBytes(memref::ViewOp view, Type world) {
  auto argument = dyn_cast<BlockArgument>(view.getSource());
  if (!world || !argument || argument.getType() != world ||
      !isa<func::FuncOp>(argument.getOwner()->getParentOp()))
    return std::nullopt;
  auto type = cast<MemRefType>(view.getType());
  if (!type.hasStaticShape() || !type.getElementType().isIntOrFloat())
    return std::nullopt;
  APInt shift;
  if (!matchPattern(view.getByteShift(), m_ConstantInt(&shift)))
    return std::nullopt;
  int64_t bytes = type.getNumElements() *
                  ((type.getElementTypeBitWidth() + 7) / 8);
  return std::make_pair(shift.getSExtValue(),
                        shift.getSExtValue() + std::max<int64_t>(bytes, 1));
}

struct EntLowerViews : public mlir::ent::impl::EntLowerViewsBase<EntLowerViews> {
  using EntLowerViewsBase::EntLowerViewsBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    MLIRContext *context = module.getContext();
    // The world's type: what a system or a schedule is given first.
    Type world;
    module.walk([&](func::FuncOp func) {
      if (!world && func->hasAttr("ent.trace") && func.getNumArguments() &&
          isa<MemRefType>(func.getArgument(0).getType()))
        world = func.getArgument(0).getType();
    });
    // The parts of the world for TBAA: the bytes of every view of it,
    // those that overlap merged (a view of all archetypes' counts and one
    // of a single count are one part), each part a type under one root.
    // (Each with whether it is of one value, a count or a unique's field,
    // or of many, rows.)
    struct Span {
      int64_t start, end;
      bool single;
    };
    SmallVector<Span> spans;
    module.walk([&](memref::ViewOp view) {
      if (auto bytes = worldBytes(view, world))
        spans.push_back({bytes->first, bytes->second,
                         cast<MemRefType>(view.getType()).getNumElements() ==
                             1});
    });
    llvm::sort(spans, [](const Span &a, const Span &b) {
      return std::tie(a.start, a.end) < std::tie(b.start, b.end);
    });
    SmallVector<Span> parts;
    for (Span span : spans)
      if (!parts.empty() && span.start < parts.back().end) {
        parts.back().end = std::max(parts.back().end, span.end);
        parts.back().single &= span.single;
      } else {
        parts.push_back(span);
      }
    // A part of single values is a type of its own; all the rows are one
    // type. So LLVM keeps a count or a unique's field in a register past
    // the stores to rows, but does not take rows of one buffer for apart
    // from another's: that let it make a loop of scattered writes into
    // gathers and scatters, slower here than one at a time (entsat).
    auto root = LLVM::TBAARootAttr::get(
        context, StringAttr::get(context, "ent world"));
    auto typeNamed = [&](const llvm::Twine &name) {
      return LLVM::TBAATypeDescriptorAttr::get(
          context, name.str(), {LLVM::TBAAMemberAttr::get(root, 0)});
    };
    auto tagOfType = [&](LLVM::TBAATypeDescriptorAttr type) {
      return ArrayAttr::get(context, {LLVM::TBAATagAttr::get(type, type, 0)});
    };
    ArrayAttr rows = tagOfType(typeNamed("ent world rows"));
    SmallVector<ArrayAttr> tags;
    for (const Span &part : parts)
      tags.push_back(part.single
                         ? tagOfType(typeNamed("ent world " +
                                               llvm::Twine(part.start)))
                         : rows);
    // The tag of a view's loads and stores: of the part its bytes are in.
    auto tagOf = [&](memref::ViewOp view) -> ArrayAttr {
      auto bytes = tbaa ? worldBytes(view, world) : std::nullopt;
      if (!bytes)
        return nullptr;
      auto part = llvm::upper_bound(
          parts, bytes->first,
          [](int64_t at, const Span &part) { return at < part.start; });
      return tags[part - parts.begin() - 1];
    };
    module.walk([&](func::FuncOp func) {
      if (!func.isExternal())
        lower(func, tagOf);
    });
  }

  void lower(func::FuncOp func,
             llvm::function_ref<ArrayAttr(memref::ViewOp)> tagOf) {
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
      ArrayAttr tag = tagOf(view);
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
          auto value = LLVM::LoadOp::create(builder, at, element, place);
          if (tag)
            value.setTbaaAttr(tag);
          load.getResult().replaceAllUsesWith(value.getResult());
          load.erase();
        } else {
          auto store = cast<memref::StoreOp>(user);
          Value place = LLVM::GEPOp::create(
              builder, at, pointer, element, column,
              ArrayRef<LLVM::GEPArg>{asI64(at, store.getIndices()[0])});
          auto stored = LLVM::StoreOp::create(builder, at,
                                              store.getValueToStore(), place);
          if (tag)
            stored.setTbaaAttr(tag);
          store.erase();
        }
      }
      if (view->use_empty())
        view.erase();
    }
  }
};

} // namespace
