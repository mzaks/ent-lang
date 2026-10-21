#include "Ent/Passes.h"

#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Path.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTTRACE
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// The .ent file and line `loc` is of (the first it holds), or "".
static std::string placeOf(Location loc) {
  std::string place;
  loc->walk([&](Location inner) {
    auto file = dyn_cast<FileLineColLoc>(inner);
    if (!file)
      return WalkResult::advance();
    place = (llvm::sys::path::filename(file.getFilename().getValue()) + ":" +
             Twine(file.getLine()))
                .str();
    return WalkResult::interrupt();
  });
  return place;
}

struct EntTrace : public mlir::ent::impl::EntTraceBase<EntTrace> {
  void runOnOperation() override {
    ModuleOp module = getOperation();
    MLIRContext *context = module.getContext();
    OpBuilder builder(context);
    Type i64 = builder.getI64Type();
    Type pointer = LLVM::LLVMPointerType::get(context);
    SymbolTable symbols(module);
    auto declare = [&](StringRef name, ArrayRef<Type> inputs,
                       ArrayRef<Type> results) {
      if (symbols.lookup(name))
        return;
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(module.getBody());
      auto func =
          func::FuncOp::create(builder, module.getLoc(), name,
                               builder.getFunctionType(inputs, results));
      func.setPrivate();
      symbols.insert(func);
    };
    declare("ent_trace_start", {}, {});
    declare("ent_trace_name", {i64, pointer}, {});
    declare("ent_trace_begin", {i64}, {});
    declare("ent_trace_end", {i64}, {});
    declare("ent_trace_loop_begin", {i64}, {});
    declare("ent_trace_loop_end", {i64}, {});
    declare("ent_trace_now", {}, {i64});
    declare("ent_trace_work", {i64, i64}, {});

    // Each site a number, and what it is called.
    SmallVector<std::string> names;
    auto site = [&](OpBuilder &at, Location loc, std::string name) {
      Value number = arith::ConstantIntOp::create(at, loc, names.size(), 64);
      names.push_back(std::move(name));
      return number;
    };
    auto call = [&](OpBuilder &at, Location loc, StringRef name,
                    ValueRange args) {
      auto callee = symbols.lookup<func::FuncOp>(name);
      return func::CallOp::create(at, loc, callee, args);
    };

    module.walk([&](func::FuncOp func) {
      if (func.isExternal())
        return;
      // A system's, a schedule's or a sort's: the whole of it.
      if (auto kind = func->getAttrOfType<StringAttr>("ent.trace")) {
        Location loc = func.getLoc();
        builder.setInsertionPointToStart(&func.getBody().front());
        std::string name = (kind.getValue() + " " + func.getSymName()).str();
        std::string place = placeOf(loc);
        if (!place.empty())
          name += " (" + place + ")";
        Value number = site(builder, loc, name);
        call(builder, loc, "ent_trace_begin", {number});
        func.walk([&](func::ReturnOp ret) {
          builder.setInsertionPoint(ret);
          call(builder, ret.getLoc(), "ent_trace_end", {number});
        });
      }
      // A parallel loop's: where it is entered and left, and each
      // thread's time in it.
      func.walk([&](omp::ParallelOp parallel) {
        Location loc = parallel.getLoc();
        std::string place = placeOf(loc);
        builder.setInsertionPoint(parallel);
        Value number =
            site(builder, loc,
                 "loop " + (place.empty() ? std::string("?") : place) + " in " +
                     func.getSymName().str());
        call(builder, loc, "ent_trace_loop_begin", {number});
        builder.setInsertionPointAfter(parallel);
        call(builder, loc, "ent_trace_loop_end", {number});
        Region &region = parallel.getRegion();
        builder.setInsertionPointToStart(&region.front());
        Value start = call(builder, loc, "ent_trace_now", {}).getResult(0);
        for (Block &block : region)
          if (auto end = dyn_cast<omp::TerminatorOp>(block.getTerminator())) {
            builder.setInsertionPoint(end);
            call(builder, loc, "ent_trace_work", {number, start});
          }
      });
    });

    // (Sites that would have the same name, a sort's loops say, are told
    // apart by their order: #1, #2, ...)
    llvm::StringMap<int> uses, seen;
    for (const std::string &name : names)
      uses[name] += 1;
    for (std::string &name : names)
      if (uses[name] > 1)
        name += " #" + std::to_string(++seen[name]);

    // `main` starts the trace and names the sites.
    auto main = symbols.lookup<func::FuncOp>("main");
    if (!main || main.isExternal())
      return;
    Location loc = main.getLoc();
    builder.setInsertionPointToStart(&main.getBody().front());
    call(builder, loc, "ent_trace_start", {});
    for (auto [number, name] : llvm::enumerate(names)) {
      Value text = LLVM::createGlobalString(
          loc, builder, ("ent_trace_site_" + Twine(number)).str(), name + '\0',
          LLVM::Linkage::Internal);
      call(builder, loc, "ent_trace_name",
           {arith::ConstantIntOp::create(builder, loc, number, 64), text});
    }
  }
};

} // namespace
