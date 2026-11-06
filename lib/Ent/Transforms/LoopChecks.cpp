#include "Ent/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTLOOPCHECKS
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// A buffer's count: one i64 of the world (`memref<1xi64>`, a view of a
/// function's argument at a constant byte), known by the argument and
/// the byte.
using Count = std::pair<Value, int64_t>;

static std::optional<Count> countOf(Value memref) {
  auto view = memref.getDefiningOp<memref::ViewOp>();
  if (!view)
    return std::nullopt;
  auto type = cast<MemRefType>(view.getType());
  if (!type.hasStaticShape() || type.getNumElements() != 1 ||
      !type.getElementType().isInteger(64) ||
      !isa<BlockArgument>(view.getSource()))
    return std::nullopt;
  APInt shift;
  if (!matchPattern(view.getByteShift(), m_ConstantInt(&shift)))
    return std::nullopt;
  return Count{view.getSource(), shift.getSExtValue()};
}

/// The count `value` is loaded from, if it is a load of one.
static std::optional<Count> loadedCount(Value value) {
  auto load = value.getDefiningOp<memref::LoadOp>();
  if (!load)
    return std::nullopt;
  return countOf(load.getMemRef());
}

static Value throughCasts(Value value) {
  while (Operation *op = value.getDefiningOp()) {
    if (!isa<arith::IndexCastOp, arith::IndexCastUIOp, arith::ExtSIOp,
             arith::ExtUIOp>(op))
      break;
    value = op->getOperand(0);
  }
  return value;
}

static bool isConstantAtLeast(Value value, int64_t least) {
  APInt constant;
  // (Through sign-keeping casts: a bound is often an i32 made an index.)
  while (auto cast = value.getDefiningOp<arith::IndexCastOp>())
    value = cast.getIn();
  while (auto wider = value.getDefiningOp<arith::ExtSIOp>())
    value = wider.getIn();
  return matchPattern(value, m_ConstantInt(&constant)) &&
         constant.getSExtValue() >= least;
}

/// Whether `op` may change the count `count`: a store to it, or a call
/// given the memory it is in.
static bool mayChange(Operation *op, const Count &count) {
  bool changes = false;
  op->walk([&](Operation *inner) {
    if (auto store = dyn_cast<memref::StoreOp>(inner)) {
      if (auto stored = countOf(store.getMemRef()))
        changes |= *stored == count;
      else
        changes |= store.getMemRef() == count.first;
    } else if (isa<func::CallOp, func::CallIndirectOp, LLVM::CallOp>(inner)) {
      changes |= llvm::is_contained(inner->getOperands(), count.first);
    }
    return changes ? WalkResult::interrupt() : WalkResult::advance();
  });
  return changes;
}

/// A loop that counts: from `lower` (≥ 0) by one up to below `upper`, an
/// `scf.for`, or the `scf.while` a `for` with a `break` becomes (a
/// counter compared with `upper` before each turn, perhaps `and` a flag,
/// and one more after it).
struct Counted {
  Operation *loop;
  Value counter; // in the loop's body
  Value lower, upper;
};

static std::optional<Counted> countedWhile(scf::WhileOp loop, unsigned k) {
  Block &before = loop.getBefore().front();
  Block &after = loop.getAfter().front();
  auto condition = loop.getConditionOp();
  if (k >= condition.getArgs().size() ||
      condition.getArgs()[k] != before.getArgument(k))
    return std::nullopt;
  // The turn is taken only where the counter is below the bound.
  Value upper;
  SmallVector<Value> conjuncts{condition.getCondition()};
  while (!conjuncts.empty() && !upper) {
    Value value = conjuncts.pop_back_val();
    if (auto both = value.getDefiningOp<arith::AndIOp>()) {
      conjuncts.push_back(both.getLhs());
      conjuncts.push_back(both.getRhs());
    } else if (auto compare = value.getDefiningOp<arith::CmpIOp>()) {
      if (compare.getPredicate() == arith::CmpIPredicate::slt &&
          compare.getLhs() == before.getArgument(k) &&
          compare.getRhs().getParentRegion()->isProperAncestor(
              &loop.getBefore()))
        upper = compare.getRhs();
    }
  }
  if (!upper || !upper.getType().isIndex())
    return std::nullopt;
  auto yield = cast<scf::YieldOp>(after.getTerminator());
  auto next = yield.getOperand(k).getDefiningOp<arith::AddIOp>();
  if (!next || !((next.getLhs() == after.getArgument(k) &&
                  isConstantAtLeast(next.getRhs(), 1)) ||
                 (next.getRhs() == after.getArgument(k) &&
                  isConstantAtLeast(next.getLhs(), 1))))
    return std::nullopt;
  return Counted{loop, after.getArgument(k), loop.getInits()[k], upper};
}

/// The counted loop `value` is the counter of, if it is one.
static std::optional<Counted> counterOf(Value value) {
  auto argument = dyn_cast<BlockArgument>(value);
  if (!argument)
    return std::nullopt;
  Operation *owner = argument.getOwner()->getParentOp();
  if (auto loop = dyn_cast<scf::ForOp>(owner)) {
    if (argument != loop.getInductionVar())
      return std::nullopt;
    return Counted{loop, argument, loop.getLowerBound(),
                   loop.getUpperBound()};
  }
  if (auto loop = dyn_cast<scf::WhileOp>(owner))
    if (argument.getOwner() == &loop.getAfter().front())
      return countedWhile(loop, argument.getArgNumber());
  return std::nullopt;
}

/// `upper` is at most the count `count` (as loaded before the loop): its
/// value as an index, or as an i32 first (a count's low half, as
/// `len()` gives it: never more than the count).
static bool atMostCount(Value upper, const Count &count, Operation *loop) {
  Value value = upper;
  if (auto cast = value.getDefiningOp<arith::IndexCastOp>())
    value = cast.getIn();
  else
    return false;
  if (auto narrow = value.getDefiningOp<arith::TruncIOp>()) {
    if (!narrow.getType().isInteger(32))
      return false;
    value = narrow.getIn();
  }
  auto load = value.getDefiningOp<memref::LoadOp>();
  if (!load || countOf(load.getMemRef()) != count)
    return false;
  // Nothing between that load and the loop, or in it, changes the count.
  if (load->getBlock() != loop->getBlock() || !load->isBeforeInBlock(loop))
    return false;
  for (Operation *op = load->getNextNode(); op; op = op->getNextNode()) {
    if (mayChange(op, count))
      return false;
    if (op == loop)
      return true;
  }
  return false;
}

static bool insideParallel(Operation *op) {
  for (Operation *parent = op->getParentOp(); parent;
       parent = parent->getParentOp())
    if (isa<scf::ParallelOp, scf::ForallOp, omp::WsloopOp,
            omp::ParallelOp>(parent))
      return true;
  return false;
}

/// Read guards that cannot fail: `row < count`, where the row is the
/// counter of a loop from zero (or more) to below the count, and nothing
/// in the loop changes that count. Each becomes `true`, and LLVM drops
/// the selects that used it.
static unsigned dropGuards(ModuleOp module) {
  SmallVector<arith::CmpIOp> guards;
  module.walk([&](arith::CmpIOp compare) {
    if (compare.getPredicate() != arith::CmpIPredicate::ult)
      return;
    auto count = loadedCount(compare.getRhs());
    if (!count)
      return;
    auto loop = counterOf(throughCasts(compare.getLhs()));
    if (!loop || !loop->loop->isProperAncestor(compare) ||
        !isConstantAtLeast(loop->lower, 0) ||
        !atMostCount(loop->upper, *count, loop->loop) ||
        mayChange(loop->loop, *count))
      return;
    guards.push_back(compare);
  });
  for (arith::CmpIOp compare : guards) {
    OpBuilder builder(compare);
    compare.replaceAllUsesWith(
        arith::ConstantIntOp::create(builder, compare.getLoc(), 1, 1)
            .getResult());
    compare.erase();
  }
  return guards.size();
}

/// The check of an append (`+=`) a row more: `count < capacity` asserted,
/// what the capacity is and of which count.
struct RoomCheck {
  cf::AssertOp check;
  Count count;
  int64_t capacity;
};

static std::optional<RoomCheck> roomCheck(cf::AssertOp check) {
  if (!check.getMsg().starts_with("no room for another row: the buffer @"))
    return std::nullopt;
  auto compare = check.getArg().getDefiningOp<arith::CmpIOp>();
  APInt capacity;
  if (!compare || compare.getPredicate() != arith::CmpIPredicate::ult ||
      !matchPattern(compare.getRhs(), m_ConstantInt(&capacity)))
    return std::nullopt;
  auto count = loadedCount(throughCasts(compare.getLhs()));
  if (!count)
    return std::nullopt;
  return RoomCheck{check, *count, capacity.getSExtValue()};
}

/// The counts a loop's turn appends to, each with how many rows a turn
/// appends at most and the capacity; none where the loop is not one to
/// check once (a count otherwise changed, a loop inside that appends).
struct Appends {
  llvm::MapVector<Count, std::pair<int64_t, int64_t>> counts;
  SmallVector<cf::AssertOp> checks;
};

static Region &bodyOf(Operation *loop) {
  if (auto turning = dyn_cast<scf::WhileOp>(loop))
    return turning.getAfter();
  return loop->getRegion(0);
}

static std::optional<Appends> appendsOf(Operation *loop) {
  Appends appends;
  bool fits = true;
  Region &body = bodyOf(loop);
  if (isa<scf::WhileOp>(loop)) {
    // (The condition's block only compares.)
    cast<scf::WhileOp>(loop).getBefore().walk([&](Operation *op) {
      fits &= !isa<memref::StoreOp, cf::AssertOp, func::CallOp,
                   LLVM::CallOp>(op);
    });
  }
  body.walk([&](cf::AssertOp check) {
    if (auto room = roomCheck(check)) {
      appends.counts[room->count].second = room->capacity;
      appends.checks.push_back(check);
    }
  });
  if (!fits)
    return std::nullopt;
  // Each store to such a count is one more than it was, in the loop's own
  // turn (not a loop inside it); a call given the world may change any.
  body.walk([&](Operation *op) {
    if (isa<func::CallOp, func::CallIndirectOp, LLVM::CallOp>(op)) {
      for (auto &[count, _] : appends.counts)
        fits &= !llvm::is_contained(op->getOperands(), count.first);
      return;
    }
    auto store = dyn_cast<memref::StoreOp>(op);
    if (!store)
      return;
    auto count = countOf(store.getMemRef());
    if (!count || !appends.counts.count(*count))
      return;
    auto more = store.getValue().getDefiningOp<arith::AddIOp>();
    bool one = more && loadedCount(more.getLhs()) == count &&
               isConstantAtLeast(more.getRhs(), 1) &&
               !isConstantAtLeast(more.getRhs(), 2);
    for (Operation *parent = op->getParentOp(); parent != loop;
         parent = parent->getParentOp())
      one &= !isa<LoopLikeOpInterface>(parent);
    fits &= one;
    appends.counts[*count].first += 1;
  });
  // Each check is of a count only appended to like that.
  for (auto &[count, rows] : appends.counts)
    fits &= rows.first > 0;
  if (!fits)
    return std::nullopt;
  return appends;
}

/// The least and the most a value may be in a loop, as i64 values made
/// before it.
struct Range {
  Value least, most;
};

/// The range of `value` in `loop`, where it is made by `+`, `-`, `*` and
/// casts of: values made before the loop, constants, and the counters of
/// the loop and of counted loops inside it whose bounds have ranges.
/// With no `build`, only whether there is one. Each part of it must be an
/// i32's (`fits`, asked before the loop): so nothing wraps, as i32 or as
/// the i64 here.
class Ranges {
public:
  Ranges(Operation *loop, OpBuilder *build) : loop(loop), build(build) {}

  std::optional<Range> of(Value value) {
    if (auto known = ranges.find(value); known != ranges.end())
      return known->second;
    auto range = compute(value);
    if (range)
      ranges[value] = *range;
    return range;
  }

  Value constant(int64_t value) {
    return arith::ConstantIntOp::create(*build, loop->getLoc(), value, 64);
  }

  SmallVector<Value> fits;

private:
  Operation *loop;
  OpBuilder *build;
  DenseMap<Value, Range> ranges;

  bool before(Value value) {
    Region *region = value.getParentRegion();
    return !loop->isAncestor(region->getParentOp()) &&
           region->isAncestor(loop->getParentRegion());
  }

  Value wide(Value value) {
    Location loc = loop->getLoc();
    auto i64 = build->getI64Type();
    if (value.getType().isIndex())
      return arith::IndexCastOp::create(*build, loc, i64, value);
    if (value.getType().getIntOrFloatBitWidth() < 64)
      return arith::ExtSIOp::create(*build, loc, i64, value);
    return value;
  }

  std::optional<Range> made(Value least, Value most) {
    if (!build)
      return Range{};
    Location loc = loop->getLoc();
    fits.push_back(arith::AndIOp::create(
        *build, loc,
        arith::CmpIOp::create(*build, loc, arith::CmpIPredicate::sge, least,
                              constant(INT32_MIN)),
        arith::CmpIOp::create(*build, loc, arith::CmpIPredicate::sle, most,
                              constant(INT32_MAX))));
    return Range{least, most};
  }

  std::optional<Range> compute(Value value) {
    Type type = value.getType();
    if (!type.isIndex() &&
        !(type.isInteger() && type.getIntOrFloatBitWidth() > 1 &&
          type.getIntOrFloatBitWidth() <= 64))
      return std::nullopt;
    if (before(value)) {
      if (!build)
        return Range{};
      Value wider = wide(value);
      return made(wider, wider);
    }
    if (auto counted = counterOf(value)) {
      // (Its bounds by their own ranges: a constant made in the loop, or
      // a value from before it.)
      if (!loop->isAncestor(counted->loop))
        return std::nullopt;
      auto lower = of(counted->lower);
      if (!lower)
        return std::nullopt;
      auto upper = of(counted->upper);
      if (!upper)
        return std::nullopt;
      if (!build)
        return Range{};
      Location loc = loop->getLoc();
      Value least = lower->least;
      Value last =
          arith::SubIOp::create(*build, loc, upper->most, constant(1));
      return made(least, arith::MaxSIOp::create(*build, loc, least, last));
    }
    Operation *op = value.getDefiningOp();
    if (!op)
      return std::nullopt;
    APInt known;
    if (matchPattern(value, m_ConstantInt(&known))) {
      // (Made again before the loop, as an i64.)
      if (!build)
        return Range{};
      Value wider = constant(known.getSExtValue());
      return made(wider, wider);
    }
    if (isa<arith::ExtSIOp, arith::IndexCastOp, arith::TruncIOp>(op))
      return of(op->getOperand(0)); // (Within an i32's: as it was.)
    if (isa<arith::ExtUIOp, arith::IndexCastUIOp>(op)) {
      auto from = of(op->getOperand(0));
      if (from && build)
        fits.push_back(arith::CmpIOp::create(
            *build, loop->getLoc(), arith::CmpIPredicate::sge, from->least,
            constant(0)));
      return from;
    }
    if (!isa<arith::AddIOp, arith::SubIOp, arith::MulIOp>(op))
      return std::nullopt;
    auto left = of(op->getOperand(0));
    if (!left)
      return std::nullopt;
    auto right = of(op->getOperand(1));
    if (!right)
      return std::nullopt;
    if (!build)
      return Range{};
    Location loc = loop->getLoc();
    auto add = [&](Value a, Value b) {
      return arith::AddIOp::create(*build, loc, a, b).getResult();
    };
    auto sub = [&](Value a, Value b) {
      return arith::SubIOp::create(*build, loc, a, b).getResult();
    };
    if (isa<arith::AddIOp>(op))
      return made(add(left->least, right->least),
                  add(left->most, right->most));
    if (isa<arith::SubIOp>(op))
      return made(sub(left->least, right->most),
                  sub(left->most, right->least));
    Value products[] = {
        arith::MulIOp::create(*build, loc, left->least, right->least),
        arith::MulIOp::create(*build, loc, left->least, right->most),
        arith::MulIOp::create(*build, loc, left->most, right->least),
        arith::MulIOp::create(*build, loc, left->most, right->most)};
    Value least = products[0], most = products[0];
    for (Value product : ArrayRef(products).drop_front()) {
      least = arith::MinSIOp::create(*build, loc, least, product);
      most = arith::MaxSIOp::create(*build, loc, most, product);
    }
    return made(least, most);
  }
};

/// The read guards in `loop` (`row < count`, the count not changed in the
/// loop) whose row has a range there.
static SmallVector<arith::CmpIOp> rangedGuards(Operation *loop) {
  SmallVector<arith::CmpIOp> guards;
  Ranges ranges(loop, nullptr);
  bodyOf(loop).walk([&](arith::CmpIOp compare) {
    if (compare.getPredicate() != arith::CmpIPredicate::ult)
      return;
    auto count = loadedCount(compare.getRhs());
    if (!count || mayChange(loop, *count) ||
        !ranges.of(compare.getLhs()))
      return;
    guards.push_back(compare);
  });
  return guards;
}

static std::optional<Counted> countedLoop(Operation *op) {
  if (auto loop = dyn_cast<scf::ForOp>(op)) {
    if (isConstantAtLeast(loop.getStep(), 1) &&
        !isConstantAtLeast(loop.getStep(), 2))
      return Counted{loop, loop.getInductionVar(), loop.getLowerBound(),
                     loop.getUpperBound()};
    return std::nullopt;
  }
  if (auto turning = dyn_cast<scf::WhileOp>(op))
    for (unsigned k = 0; k < turning.getAfter().getNumArguments(); ++k)
      if (turning.getAfter().getArgument(k).getType().isIndex())
        if (auto counted = countedWhile(turning, k))
          return counted;
  return std::nullopt;
}

/// Loops checked once. Before the loop: whether there is room for as many
/// rows as its turns could append at most (the turns it would take at
/// most, times the appends in a turn), and whether the rows it reads are
/// within their counts (from the ranges of the rows). Then the loop
/// without those checks where so, and as it was where not: a program out
/// of room still stops at the row it would have, with the same words. In
/// the copy, the counts appended to are kept in a value of the function's
/// own, which LLVM keeps in a register, and written back after it.
///
/// The loops are the innermost that have such checks, but not one of a
/// constant number of turns (whose counter is a range in the loop around
/// it).
static unsigned checkOnce(ModuleOp module) {
  struct Chosen {
    Counted counted;
    Appends appends;
    SmallVector<arith::CmpIOp> guards;
  };
  SmallVector<Chosen> loops;
  DenseSet<Operation *> chosen;
  module.walk([&](Operation *op) {
    if (!isa<scf::ForOp, scf::WhileOp>(op) || insideParallel(op))
      return;
    auto counted = countedLoop(op);
    if (!counted || !counted->lower.getType().isIndex() ||
        isConstantAtLeast(counted->upper, INT64_MIN))
      return;
    bool inner = false;
    op->walk([&](Operation *inside) {
      inner |= inside != op && chosen.contains(inside);
    });
    if (inner)
      return;
    auto appends = appendsOf(op);
    if (!appends)
      appends = Appends{};
    auto guards = rangedGuards(op);
    if (appends->checks.empty() && guards.empty())
      return;
    chosen.insert(op);
    loops.push_back({*counted, std::move(*appends), std::move(guards)});
  });
  for (auto &[counted, appends, guards] : loops) {
    Operation *loop = counted.loop;
    Location loc = loop->getLoc();
    OpBuilder builder(loop);
    auto i64 = builder.getI64Type();
    Ranges ranges(loop, &builder);
    auto constant = [&](int64_t value) { return ranges.constant(value); };
    auto countView = [&](const Count &count) {
      return memref::ViewOp::create(
          builder, loc, MemRefType::get({1}, i64), count.first,
          arith::ConstantIndexOp::create(builder, loc, count.second),
          ValueRange{});
    };
    auto zero = [&] { return arith::ConstantIndexOp::create(builder, loc, 0); };
    SmallVector<Value> conditions;
    // The turns: upper - lower, or none.
    Value turns = arith::SubIOp::create(
        builder, loc, arith::IndexCastOp::create(builder, loc, i64, counted.upper),
        arith::IndexCastOp::create(builder, loc, i64, counted.lower));
    turns = arith::MaxSIOp::create(builder, loc, turns, constant(0));
    SmallVector<std::pair<Count, Value>> now;
    for (auto &[count, rows] : appends.counts) {
      Value value = memref::LoadOp::create(builder, loc, countView(count),
                                           ValueRange{zero()});
      now.push_back({count, value});
      // turns <= (capacity - now) / rows, with now <= capacity always.
      Value left = arith::DivSIOp::create(
          builder, loc,
          arith::SubIOp::create(builder, loc, constant(rows.second), value),
          constant(rows.first));
      conditions.push_back(arith::CmpIOp::create(
          builder, loc, arith::CmpIPredicate::sle, turns, left));
    }
    for (arith::CmpIOp guard : guards) {
      Range row = *ranges.of(guard.getLhs());
      auto count = loadedCount(guard.getRhs());
      Value rows = memref::LoadOp::create(builder, loc, countView(*count),
                                          ValueRange{zero()});
      conditions.push_back(arith::CmpIOp::create(
          builder, loc, arith::CmpIPredicate::sge, row.least, constant(0)));
      conditions.push_back(arith::CmpIOp::create(
          builder, loc, arith::CmpIPredicate::slt, row.most, rows));
    }
    conditions.append(ranges.fits);
    Value all = conditions.front();
    for (Value condition : ArrayRef(conditions).drop_front())
      all = arith::AndIOp::create(builder, loc, all, condition);
    auto choice = scf::IfOp::create(builder, loc, loop->getResultTypes(), all,
                                    /*withElseRegion=*/true);
    // (Without results the blocks have their yields already.)
    auto yieldIn = [&](Block *block, ValueRange values) {
      if (!block->empty() && isa<scf::YieldOp>(block->back()))
        block->back().erase();
      builder.setInsertionPointToEnd(block);
      return scf::YieldOp::create(builder, loc, values);
    };
    // Where so: a copy of the loop without the checks, its counts local.
    IRMapping mapping;
    builder.setInsertionPointToStart(choice.thenBlock());
    SmallVector<std::pair<Count, Value>> locals;
    {
      auto entry = OpBuilder::atBlockBegin(
          &loop->getParentOfType<func::FuncOp>().getBody().front());
      for (auto &[count, value] : now) {
        Value local = memref::AllocaOp::create(entry, loc,
                                               MemRefType::get({1}, i64));
        memref::StoreOp::create(builder, loc, value, local,
                                ValueRange{zero()});
        locals.push_back({count, local});
      }
    }
    Operation *copy = builder.clone(*loop, mapping);
    for (auto &kept : locals) {
      Count count = kept.first;
      Value local = kept.second;
      memref::StoreOp::create(
          builder, loc,
          memref::LoadOp::create(builder, loc, local, ValueRange{zero()}),
          countView(count), ValueRange{zero()});
      copy->walk([&](Operation *op) {
        Value memref;
        if (auto load = dyn_cast<memref::LoadOp>(op))
          memref = load.getMemRef();
        else if (auto store = dyn_cast<memref::StoreOp>(op))
          memref = store.getMemRef();
        if (memref && countOf(memref) == count)
          op->replaceUsesOfWith(memref, local);
      });
    }
    yieldIn(choice.thenBlock(), copy->getResults());
    for (cf::AssertOp check : appends.checks)
      mapping.lookup(check.getOperation())->erase();
    for (arith::CmpIOp guard : guards) {
      Operation *copied = mapping.lookup(guard.getOperation());
      OpBuilder at(copied);
      copied->replaceAllUsesWith(
          arith::ConstantIntOp::create(at, loc, 1, 1)->getResults());
      copied->erase();
    }
    // Where not: the loop as it was.
    auto yield = yieldIn(choice.elseBlock(), loop->getResults());
    for (auto [result, picked] :
         llvm::zip(loop->getResults(), choice.getResults()))
      result.replaceAllUsesExcept(picked, yield);
    loop->moveBefore(yield);
  }
  return loops.size();
}

struct EntLoopChecks : public mlir::ent::impl::EntLoopChecksBase<EntLoopChecks> {
  using EntLoopChecksBase::EntLoopChecksBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (guards)
      dropGuards(module);
    if (once)
      checkOnce(module);
  }
};

} // namespace
