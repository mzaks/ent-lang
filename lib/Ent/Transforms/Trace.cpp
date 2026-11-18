#include "Ent/Passes.h"

#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
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

/// The site of a loop over entities (ent-lower-to-loops puts it in the
/// loop's location, `{site = ..}`), or "".
static std::string siteOf(Location loc) {
  std::string site;
  loc->walk([&](Location inner) {
    auto fused = dyn_cast<FusedLoc>(inner);
    auto marks =
        fused ? dyn_cast_or_null<DictionaryAttr>(fused.getMetadata()) : nullptr;
    if (auto named = marks ? marks.getAs<StringAttr>("site") : nullptr) {
      site = named.getValue().str();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return site;
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
    declare("ent_trace_key", {i64, pointer}, {});
    declare("ent_trace_count", {i64, i64}, {});
    declare("ent_trace_tangled", {i64, i64, i64, i64}, {i64});
    declare("ent_trace_begin", {i64}, {});
    declare("ent_trace_end", {i64}, {});
    declare("ent_trace_loop_begin", {i64}, {});
    declare("ent_trace_loop_end", {i64}, {});
    declare("ent_trace_now", {}, {i64});
    declare("ent_trace_work", {i64, i64}, {});
    declare("ent_trace_entities", {i64, i64, i64}, {});
    declare("ent_trace_archetypes", {pointer}, {});

    // The schedules `main` runs: after each, how many entities each
    // archetype has (the counts at the start of the world, by the names
    // ent-lower-to-loops leaves of them).
    auto archetypes = module->getAttrOfType<ArrayAttr>("ent.archetypes");
    llvm::StringSet<> fromMain;
    // (Through the functions between, `ent.main` say, that are not timed.)
    SmallVector<func::FuncOp> between;
    if (auto main = symbols.lookup<func::FuncOp>("main"))
      between.push_back(main);
    llvm::StringSet<> looked;
    while (!between.empty()) {
      func::FuncOp caller = between.pop_back_val();
      caller.walk([&](func::CallOp called) {
        StringRef name = called.getCallee();
        fromMain.insert(name);
        auto callee = symbols.lookup<func::FuncOp>(name);
        if (callee && !callee.isExternal() && !callee->hasAttr("ent.trace") &&
            looked.insert(name).second)
          between.push_back(callee);
      });
    }

    // Each site a number, and what it is called; a loop over entities also
    // its site and how it ran ("site|parallel|dynamic", "site|serial"), for
    // `ent build --tune` to read.
    SmallVector<std::string> names, keys;
    auto site = [&](OpBuilder &at, Location loc, std::string name,
                    std::string key = "") {
      Value number = arith::ConstantIntOp::create(at, loc, names.size(), 64);
      names.push_back(std::move(name));
      keys.push_back(std::move(key));
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
        Value world = func.getNumArguments() ? func.getArgument(0) : Value();
        auto worldType =
            world ? dyn_cast<MemRefType>(world.getType()) : MemRefType();
        bool counted = kind.getValue() == "schedule" && archetypes &&
                       !archetypes.empty() &&
                       fromMain.contains(func.getSymName()) && worldType &&
                       worldType.getRank() == 1 &&
                       worldType.getElementType().isInteger(8);
        func.walk([&](func::ReturnOp ret) {
          builder.setInsertionPoint(ret);
          Location at = ret.getLoc();
          if (counted) {
            auto countsType = MemRefType::get(
                {static_cast<int64_t>(archetypes.size())}, i64);
            Value zero = arith::ConstantIndexOp::create(builder, at, 0);
            Value counts = memref::ViewOp::create(builder, at, countsType,
                                                  world, zero, ValueRange{});
            Value address =
                memref::ExtractAlignedPointerAsIndexOp::create(builder, at,
                                                               counts);
            call(builder, at, "ent_trace_entities",
                 {number,
                  arith::IndexCastOp::create(builder, at, i64, address),
                  arith::ConstantIntOp::create(builder, at, archetypes.size(),
                                               64)});
          }
          call(builder, at, "ent_trace_end", {number});
        });
      }
      // A parallel loop's: where it is entered and left, and each
      // thread's time in it.
      func.walk([&](omp::ParallelOp parallel) {
        Location loc = parallel.getLoc();
        std::string place = placeOf(loc);
        builder.setInsertionPoint(parallel);
        std::string key = siteOf(loc);
        if (!key.empty()) {
          bool dynamic = false;
          parallel.walk([&](omp::WsloopOp loop) {
            dynamic |= loop.getScheduleKind() ==
                       omp::ClauseScheduleKind::Dynamic;
          });
          key += dynamic ? "|parallel|dynamic" : "|parallel|static";
        }
        Value number =
            site(builder, loc,
                 "loop " + (place.empty() ? std::string("?") : place) + " in " +
                     func.getSymName().str(),
                 key);
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

    // A loop over entities on one thread (where it is never on many, or
    // where it has too few entities this time): its time, as a span.
    module.walk([&](func::FuncOp func) {
      if (func.isExternal())
        return;
      func.walk([&](scf::ForOp loop) {
        std::string key = siteOf(loop.getLoc());
        if (key.empty() || loop->getParentOfType<omp::ParallelOp>())
          return;
        Location loc = loop.getLoc();
        std::string place = placeOf(loc);
        builder.setInsertionPoint(loop);
        Value number =
            site(builder, loc,
                 "loop " + (place.empty() ? std::string("?") : place) + " in " +
                     func.getSymName().str() + " (one thread)",
                 key + "|serial");
        call(builder, loc, "ent_trace_begin", {number});
        builder.setInsertionPointAfter(loop);
        call(builder, loc, "ent_trace_end", {number});
      });
    });

    // How many values were sent where they are combined: the number the
    // choosing of the way is given, ahead of it, by the site of the loops
    // that combine them (`send ..`, in the `if` the choice makes).
    module.walk([&](func::CallOp choose) {
      if (choose.getCallee() != "ent_sends_choose" ||
          choose.getNumOperands() < 3)
        return;
      std::string key;
      for (Operation *after = choose->getNextNode(); after && key.empty();
           after = after->getNextNode())
        if (isa<scf::IfOp>(after))
          after->walk([&](Operation *inside) {
            if (key.empty())
              key = siteOf(inside->getLoc());
          });
      if (key.empty())
        return;
      Location loc = choose.getLoc();
      std::string place = placeOf(loc);
      builder.setInsertionPoint(choose);
      Value number = site(builder, loc,
                          "values sent at " +
                              (place.empty() ? std::string("?") : place),
                          key + "|count");
      call(builder, loc, "ent_trace_count", {number, choose.getOperand(2)});
    });

    // Where a grid index is made again, whether it is put in order anew
    // (its `Name_tangled`, given how many rows were below the one before
    // and of how many): those two and the way it went, by the index
    // (`tangled Name`, the name as the function's).
    module.walk([&](func::CallOp tangled) {
      StringRef callee = tangled.getCallee();
      if (!callee.starts_with("ent_") || !callee.ends_with("_tangled") ||
          tangled.getNumOperands() != 2 || tangled.getNumResults() != 1)
        return;
      Location loc = tangled.getLoc();
      std::string index =
          callee.drop_front(4).drop_back(StringRef("_tangled").size()).str();
      builder.setInsertionPointAfter(tangled);
      Value number = site(builder, loc, "index " + index + " made again",
                          "tangled " + index + "|tangle");
      auto wide = [&](Value value) {
        return arith::ExtSIOp::create(builder, loc, i64, value).getResult();
      };
      Value chosen = arith::ExtUIOp::create(builder, loc, i64,
                                            tangled.getResult(0));
      auto traced = call(builder, loc, "ent_trace_tangled",
                         {number, wide(tangled.getOperand(0)),
                          wide(tangled.getOperand(1)), chosen});
      Value way = arith::TruncIOp::create(builder, loc, builder.getI1Type(),
                                          traced.getResult(0));
      tangled.getResult(0).replaceAllUsesExcept(way, chosen.getDefiningOp());
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
    if (archetypes && !archetypes.empty()) {
      std::string all;
      for (Attribute name : archetypes)
        all += cast<StringAttr>(name).getValue().str() + "\n";
      Value text = LLVM::createGlobalString(loc, builder, "ent_trace_archetype_names",
                                            all + '\0',
                                            LLVM::Linkage::Internal);
      call(builder, loc, "ent_trace_archetypes", {text});
    }
    for (auto [number, name] : llvm::enumerate(names)) {
      Value text = LLVM::createGlobalString(
          loc, builder, ("ent_trace_site_" + Twine(number)).str(), name + '\0',
          LLVM::Linkage::Internal);
      call(builder, loc, "ent_trace_name",
           {arith::ConstantIntOp::create(builder, loc, number, 64), text});
      if (keys[number].empty())
        continue;
      Value key = LLVM::createGlobalString(
          loc, builder, ("ent_trace_key_" + Twine(number)).str(),
          keys[number] + '\0', LLVM::Linkage::Internal);
      call(builder, loc, "ent_trace_key",
           {arith::ConstantIntOp::create(builder, loc, number, 64), key});
    }
  }
};

} // namespace

