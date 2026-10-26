#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir::ent {
#define GEN_PASS_DEF_ENTINDEXES
#define GEN_PASS_DEF_ENTINFERINDEXES
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// What one index is made of and what reads it, as columns, and the
/// systems that keep it.
struct Kept {
  StringAttr update, stale, moved, fresh;
  /// Which entities it holds, and the values it holds of them.
  llvm::SetVector<Column> holds;
  llvm::SetVector<Column> values;
  llvm::SetVector<Column> readBy;
};

/// The strings of an array attribute.
SmallVector<StringAttr> strings(DictionaryAttr info, StringRef name) {
  SmallVector<StringAttr> list;
  for (Attribute item : cast<ArrayAttr>(info.get(name)))
    list.push_back(cast<StringAttr>(item));
  return list;
}

Kept keptOf(BufferOp buffer, DictionaryAttr info,
            ArrayRef<ArchetypeOp> archetypes, SymbolTable &symbols) {
  MLIRContext *context = buffer.getContext();
  StringAttr empty = StringAttr::get(context, "");
  Kept kept;
  kept.update = cast<StringAttr>(info.get("update"));
  kept.stale = cast<StringAttr>(info.get("stale"));
  kept.moved = cast<StringAttr>(info.get("moved"));
  kept.fresh = cast<StringAttr>(info.get("fresh"));
  SmallVector<StringAttr> of = strings(info, "of");
  SmallVector<StringAttr> row = strings(info, "row");
  // What reads it: its rows (and a grid's cells and sizes), and the
  // fields where the entities keep their place in it.
  for (StringAttr own : strings(info, "own")) {
    Operation *declared = symbols.lookup(own);
    if (isa_and_nonnull<BufferOp>(declared))
      kept.readBy.insert(
          {StringAttr(), own, StringAttr::get(context, "rows")});
    if (auto resource = dyn_cast_or_null<ResourceOp>(declared))
      for (Attribute field : resource.getFieldNames())
        kept.readBy.insert({StringAttr(), own, cast<StringAttr>(field)});
  }
  for (ArchetypeOp archetype : archetypes) {
    StringAttr name = archetype.getSymNameAttr();
    if (archetype.contains(FlatSymbolRefAttr::get(row[0])))
      for (StringAttr hidden : strings(info, "hidden"))
        kept.readBy.insert({name, row[0], hidden});
    if (!llvm::all_of(of, [&](StringAttr component) {
          return archetype.contains(FlatSymbolRefAttr::get(component));
        }))
      continue;
    // Which entities it holds.
    kept.holds.insert({name, empty, empty});
    for (StringAttr component : of)
      if (archetype.isOptional(FlatSymbolRefAttr::get(component)))
        kept.holds.insert({name, component, empty});
  }
  // The values it holds of them: the fields its update reads, of the
  // components (but those it keeps its own in) and of the uniques it
  // does not write itself (but the flags that say it is up to date): the
  // key, what it copies, a grid's places and what its sizes are worked
  // out of. (Its update gives a proc the first component to change, which
  // the analysis takes for writing all of it.)
  SystemAccess made = computeAccess(symbols.lookup(kept.update), archetypes);
  for (const Column &column : made.reads) {
    auto [archetype, component, field] = column;
    if (kept.readBy.contains(column) || kept.holds.contains(column) ||
        !component || component.empty() || !field || field.empty() ||
        (!archetype && made.writes.contains(column)))
      continue;
    kept.values.insert(column);
  }
  return kept;
}

/// Whether `system`, but for `query`, may change the field `query`'s
/// order is by, or its key equals (`component`, `field`), or which
/// entities there are before it runs: then the index, made when the
/// system starts, is not what the `for` would sort. (Writes to the key in
/// the query itself are fine: a `for ... by` goes by the values the key
/// had when it started.)
bool changesOrderOutside(SystemOp system, QueryOp query,
                         FlatSymbolRefAttr component, StringAttr field) {
  // (What comes after the query does not change what it sees, where no
  // loop of the system's runs the two again.)
  bool once = true;
  for (Operation *around = query->getParentOp(); around != system;
       around = around->getParentOp())
    once &= !isa<scf::ForOp, scf::WhileOp>(around);
  WalkResult result = system.walk<WalkOrder::PreOrder>([&](Operation *op) {
    if (op == query.getOperation())
      return WalkResult::skip();
    if (Operation *top = query->getBlock()->findAncestorOpInBlock(*op);
        once && top && query->isBeforeInBlock(top))
      return WalkResult::skip();
    if (isa<SpawnOp, DespawnOp, AddOp, RemoveOp>(op))
      return WalkResult::interrupt();
    // (A proc changes of the world only what it is handed to change: here
    // the key's component.)
    if (auto invoke = dyn_cast<InvokeOp>(op); invoke && invoke.getProc())
      for (Value operand : invoke->getOperands()) {
        if (auto rows = dyn_cast<BufferType>(operand.getType());
            rows && rows.getMut() && rows.getName() == component)
          return WalkResult::interrupt();
        if (auto ref = dyn_cast<RefType>(operand.getType());
            ref && ref.getIsMutable() && ref.getComponent() == component)
          return WalkResult::interrupt();
      }
    if (auto set = dyn_cast<SetOp>(op))
      if (cast<RefType>(set.getRef().getType()).getComponent() == component &&
          set.getFieldAttr() == field)
        return WalkResult::interrupt();
    if (auto apply = dyn_cast<ApplyOp>(op))
      if (apply.getComponentAttr() == component &&
          apply.getFieldAttr() == field)
        return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return result.wasInterrupted();
}

/// Whether `system` writes the key of the index `kept` or a field it
/// copies (of entities that are there: not in spawning them), or hands
/// a proc one of their components. (Not a unique it is made of, such as
/// a grid's cell: the GPU is given those as it runs.)
bool writesValues(SystemOp system, const Kept &kept) {
  auto held = [&](Attribute component, StringAttr field) {
    return llvm::any_of(kept.values, [&](const Column &column) {
      return std::get<0>(column) && std::get<1>(column) == component &&
             (!field || std::get<2>(column) == field);
    });
  };
  WalkResult result = system.walk([&](Operation *op) {
    if (auto set = dyn_cast<SetOp>(op))
      if (held(cast<RefType>(set.getRef().getType()).getComponent().getAttr(),
               set.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto apply = dyn_cast<ApplyOp>(op))
      if (held(apply.getComponentAttr().getAttr(), apply.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto combine = dyn_cast<CombineOp>(op))
      if (held(cast<RefType>(combine.getRef().getType())
                   .getComponent()
                   .getAttr(),
               combine.getFieldAttr()))
        return WalkResult::interrupt();
    if (auto invoke = dyn_cast<InvokeOp>(op); invoke && invoke.getProc())
      for (Value operand : invoke->getOperands())
        if (auto ref = dyn_cast<RefType>(operand.getType());
            ref && held(ref.getComponent().getAttr(), StringAttr()))
          return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return result.wasInterrupted();
}

bool touches(const llvm::SetVector<Column> &accessed,
             const llvm::SetVector<Column> &columns) {
  return llvm::any_of(columns,
                      [&](const Column &c) { return accessed.contains(c); });
}

/// The archetypes that hold the entities of an index of `of` (each of
/// them, not optionally).
llvm::SmallDenseSet<Attribute> holdersOf(ArrayRef<ArchetypeOp> archetypes,
                                         ArrayRef<StringAttr> of) {
  llvm::SmallDenseSet<Attribute> held;
  for (ArchetypeOp archetype : archetypes)
    if (llvm::all_of(of, [&](StringAttr component) {
          return archetype.contains(FlatSymbolRefAttr::get(component)) &&
                 !archetype.isOptional(FlatSymbolRefAttr::get(component));
        }))
      held.insert(archetype.getSymNameAttr());
  return held;
}

/// Whether the body of `query` combines into what it also reads: adds to
/// a unique's field it reads, or sends to a field of a component it reads
/// or sets (of any entity). A `for` then may not go by a field (see
/// QueryOp's verifier): it is combined as the query goes there, and is
/// not visited by an index's order of its places.
bool combinesIntoWhatItReads(QueryOp query) {
  WalkResult found = query.getBody().walk([&](Operation *op) {
    bool seen = false;
    if (auto accumulate = dyn_cast<AccumulateOp>(op))
      query.getBody().walk([&](ReadOp read) {
        seen |= read.getResourceAttr() == accumulate.getResourceAttr() &&
                read.getField() == accumulate.getField();
      });
    if (auto apply = dyn_cast<ApplyOp>(op))
      query.getBody().walk([&](Operation *other) {
        FlatSymbolRefAttr component;
        StringRef field;
        if (auto get = dyn_cast<GetOp>(other)) {
          component = get.getRef().getType().getComponent();
          field = get.getField();
        } else if (auto set = dyn_cast<SetOp>(other)) {
          component = set.getRef().getType().getComponent();
          field = set.getField();
        } else if (auto lookup = dyn_cast<LookupOp>(other)) {
          if (lookup.getValue().use_empty())
            return;
          component = lookup.getComponentAttr();
          field = lookup.getField();
        } else {
          seen |= isa<AddOp>(other) &&
                  other->getAttr("component") == apply.getComponentAttr();
          return;
        }
        seen |=
            component == apply.getComponentAttr() && field == apply.getField();
      });
    return seen ? WalkResult::interrupt() : WalkResult::advance();
  });
  return found.wasInterrupted();
}

/// Whether a `for` whose `where` asks for values of a field the same for
/// all its entities (QueryOp::kKeyEqualsAttr) may visit only those an
/// index by the field (`key`), over the entities `held` has, says have
/// them: over those entities or some of them; not where the system changes the
/// key or which entities there are, but in the `for` itself and there only of
/// the entity visited (written or sent); nor where the body spawns, adds
/// or removes, or binds another entity along a tree; nor in what makes an
/// index (`making`), nor on the GPU but where `gpu` says (a partition's
/// sets, which a loop for the GPU goes through as the CPU's does).
bool answerable(QueryOp query, ArrayRef<StringAttr> key,
                const llvm::SmallDenseSet<Attribute> &held,
                const llvm::DenseSet<Attribute> &making, bool gpu = false) {
  auto equals = query->getAttrOfType<ArrayAttr>(QueryOp::kKeyEqualsAttr);
  auto system = query->getParentOfType<SystemOp>();
  // (Nor one over a set's entities, which it finds by the set.)
  if (!equals || query.getViaHash() || query.getViaPartition() ||
      query.getInSet() || !system || (system.getGpu() && !gpu) ||
      making.contains(system.getSymNameAttr()))
    return false;
  auto component = cast<FlatSymbolRefAttr>(equals[0]);
  auto field = cast<StringAttr>(equals[1]);
  if (component.getAttr() != key[0] || field != key[1])
    return false;
  // (Of some of them too: those of the others are passed over, found by
  // their ids.)
  SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
  if (matched.empty() || !llvm::all_of(matched, [&](ArchetypeOp archetype) {
        return held.contains(archetype.getSymNameAttr());
      }))
    return false;
  if (changesOrderOutside(system, query, component, field))
    return false;
  Block &body = query.getBody().front();
  // (Nor what it binds of another entity along a tree.)
  if (llvm::any_of(body.getArguments(), [](BlockArgument arg) {
        return static_cast<bool>(cast<RefType>(arg.getType()).getVia());
      }))
    return false;
  WalkResult inside = query.getBody().walk([&](Operation *op) {
    if (isa<SpawnOp, AddOp, RemoveOp>(op))
      return WalkResult::interrupt();
    // (The key written, or sent: of the entity visited only.)
    auto other = [&](Value ref, StringAttr written) {
      return cast<RefType>(ref.getType()).getComponent() == component &&
             written == field && ref.getParentBlock() != &body;
    };
    if (auto set = dyn_cast<SetOp>(op);
        set && other(set.getRef(), set.getFieldAttr()))
      return WalkResult::interrupt();
    if (auto combine = dyn_cast<CombineOp>(op);
        combine && other(combine.getRef(), combine.getFieldAttr()))
      return WalkResult::interrupt();
    if (auto apply = dyn_cast<ApplyOp>(op);
        apply && apply.getComponentAttr() == component &&
        apply.getFieldAttr() == field)
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return !inside.wasInterrupted();
}

/// Whether a `for` a partition answers may visit the entities of its bits
/// (as `for .. in Set` does: QueryOp::kInSetAttr): not one by a field,
/// in turn, by what changed or along a tree (see QueryOp's verifier).
bool overBits(QueryOp query) {
  return !query.hasBy() && !query.getCascade() && !query.isTurn() &&
         !query->hasAttr(QueryOp::kTriggersAttr);
}

/// A partition's sets (`bits`, made by `update`) made on the GPU, where
/// nothing outside what makes indexes reads the values that have any
/// (`values`, `N_kept`) and nothing in a `for` asks how many have one
/// (`N_count`): the update a `gpu` system, and each `N_count` asked a
/// count of the bits of the value's set (ent.set.count), on the GPU
/// where it is a `gpu` system's.
static void makeBitsOnGpu(ModuleOp module, BufferOp buffer, SystemOp update,
                          FlatSymbolRefAttr bits, StringAttr values,
                          const llvm::DenseSet<Attribute> &making,
                          ArrayRef<ArchetypeOp> archetypes) {
  std::string countName = (buffer.getSymName() + "_count").str();
  SmallVector<InvokeOp> counts;
  bool other = false;
  for (SystemOp system : module.getOps<SystemOp>()) {
    if (making.contains(system.getSymNameAttr()))
      continue;
    for (const Column &column : computeAccess(system, archetypes).reads)
      other |= std::get<1>(column) == values;
    system.walk([&](InvokeOp invoke) {
      if (invoke.getCallee() != countName)
        return;
      counts.push_back(invoke);
      other |= bool(invoke->getParentOfType<QueryOp>());
    });
  }
  module.walk([&](FunctionOp fn) {
    fn.walk([&](InvokeOp invoke) { other |= invoke.getCallee() == countName; });
  });
  auto counter = module.lookupSymbol<FunctionOp>(countName);
  if (other || !counter)
    return;
  update.setGpu(true);
  // (The value as the number of its set: what `N_count` makes of it
  // before it reads its row.)
  Block &body = counter.getBody().front();
  for (InvokeOp invoke : counts) {
    OpBuilder at(invoke);
    IRMapping mapping;
    mapping.map(body.getArgument(1), invoke->getOperand(1));
    Value part;
    for (Operation &op : body) {
      if (auto row = dyn_cast<BufferAtOp>(op)) {
        part = mapping.lookup(row.getIndex());
        break;
      }
      at.clone(op, mapping);
    }
    if (!part)
      continue;
    auto counted =
        SetCountOp::create(at, invoke.getLoc(), at.getI32Type(), bits, part);
    invoke->getResult(0).replaceAllUsesWith(counted.getResult());
    Operation *handed = invoke->getOperand(0).getDefiningOp();
    invoke->erase();
    if (handed && handed->use_empty())
      handed->erase();
    // (A system that says what it reads reads the bits.)
    auto system = counted->getParentOfType<SystemOp>();
    if (system.hasContract() && !system.canRead(bits)) {
      SmallVector<Attribute> reads;
      if (ArrayAttr declared = system.getReadsAttr())
        llvm::append_range(reads, declared);
      reads.push_back(bits);
      system.setReadsAttr(ArrayAttr::get(module.getContext(), reads));
    }
  }
}

struct EntIndexes : public mlir::ent::impl::EntIndexesBase<EntIndexes> {
  using EntIndexesBase::EntIndexesBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    SmallVector<std::pair<BufferOp, DictionaryAttr>> indexes;
    for (BufferOp buffer : module.getOps<BufferOp>())
      if (auto info = buffer->getAttrOfType<DictionaryAttr>("ent.index"))
        indexes.push_back({buffer, info});
    if (indexes.empty())
      return;
    // (Clustered ones first: the runs that move their rows then change
    // what the others hold, and those see them.)
    llvm::stable_sort(indexes, [](auto &a, auto &b) {
      return a.second.get("clustered") && !b.second.get("clustered");
    });
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    SmallVector<ArchetypeOp> archetypes(module.getOps<ArchetypeOp>());
    SymbolTable symbols(module);
    llvm::DenseMap<Operation *, SystemAccess> accesses;
    auto accessOf = [&](RunOp run) -> const SystemAccess & {
      Operation *system = symbols.lookup(run.getSystem());
      auto [it, inserted] = accesses.try_emplace(system);
      if (inserted)
        it->second = computeAccess(system, archetypes);
      return it->second;
    };

    // A partition (`by field partition`): the `for`s whose `where` asks
    // for values of its field that it answers (QueryOp::kViaPartitionAttr),
    // and how it keeps them. Where one archetype holds its entities, which
    // neither a sorted tree nor another clustered index keeps in order, no
    // system changes its field (an entity has its value from its spawn
    // on), and rows move anyway (entities are despawned, or given and
    // taken an `apart` component, or rows are kept in order: else every
    // entity would be found through a table, not by its number as a row,
    // which costs more than a `for` saves, see bench/epidemic), it keeps
    // the archetype's rows by its values (clustered, made where entities
    // came or went): such a `for` then visits only their rows, one after
    // another, as a `for` with an `apart` tag does those stored apart.
    // Else it keeps a set of each value's entities (its bits, mended
    // from those whose value changed only), whose words such a `for`
    // goes through, those of the values it asks for at once, as through
    // a set's (QueryOp::kInSetAttr); not one that may not (see overBits),
    // which asks every entity.
    {
      llvm::DenseSet<Attribute> making;
      for (auto [buffer, info] : indexes) {
        making.insert(info.get("update"));
        if (Attribute bits = info.get("bits_update"))
          making.insert(bits);
      }
      llvm::SmallDenseSet<Attribute> kept;
      for (auto [buffer, info] : indexes)
        if (info.get("clustered"))
          for (Attribute holder : holdersOf(archetypes, strings(info, "of")))
            kept.insert(holder);
      for (RelationOp relation : module.getOps<RelationOp>())
        if (relation.getSorted())
          if (FailureOr<SmallVector<ArchetypeOp>> sorted =
                  getSortedArchetypes(relation);
              succeeded(sorted))
            for (ArchetypeOp archetype : *sorted)
              kept.insert(archetype.getSymNameAttr());
      bool moving = !kept.empty();
      module.walk([&](Operation *op) {
        if (isa<DespawnOp>(op))
          moving = true;
        if (isa<AddOp, RemoveOp>(op))
          if (auto component = dyn_cast_or_null<ComponentOp>(symbols.lookup(
                  cast<FlatSymbolRefAttr>(op->getAttr("component")).getAttr()));
              component && component.getApart())
            moving = true;
      });
      for (auto &[buffer, info] : indexes) {
        if (!info.get("partition"))
          continue;
        SmallVector<StringAttr> key = strings(info, "key");
        llvm::SmallDenseSet<Attribute> held =
            holdersOf(archetypes, strings(info, "of"));
        SmallVector<QueryOp> answered;
        module.walk([&](QueryOp query) {
          if (answerable(query, key, held, making, /*gpu=*/true))
            answered.push_back(query);
        });
        // (What it holds as the update of its bits reads it, which hands
        // no proc the component to change: its field only.)
        auto bits = cast<StringAttr>(info.get("bits_update"));
        NamedAttrList asBits(info);
        asBits.set("update", bits);
        Kept made = keptOf(buffer, asBits.getDictionary(module.getContext()),
                           archetypes, symbols);
        int64_t changing = 0;
        for (SystemOp system : module.getOps<SystemOp>())
          if (!making.contains(system.getSymNameAttr()) &&
              writesValues(system, made))
            ++changing;
        bool clustered = !ranks && moving && changing == 0 &&
                         held.size() == 1 && !kept.contains(*held.begin()) &&
                         !answered.empty();
        // (Kept as the archetype's rows: not for a `gpu` system's `for`s,
        // nor where a `gpu` system counts the bits of a value's set.)
        if (clustered)
          llvm::erase_if(answered, [](QueryOp query) {
            return query->getParentOfType<SystemOp>().getGpu();
          });
        std::string bitsSymbol = (buffer.getSymName() + "_bits").str();
        module.walk([&](SetCountOp count) {
          auto system = count->getParentOfType<SystemOp>();
          if (count.getBuffer() == bitsSymbol && system &&
              !making.contains(system.getSymNameAttr()))
            clustered = false;
        });
        clustered &= !answered.empty();
        auto bitsName = FlatSymbolRefAttr::get(
            module.getContext(), (buffer.getSymName() + "_bits").str());
        for (QueryOp query : answered) {
          if (!clustered && !overBits(query))
            continue;
          query->setAttr(QueryOp::kViaPartitionAttr,
                         FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
          if (clustered)
            continue;
          query->setAttr(QueryOp::kInSetAttr, bitsName);
          // (A system that says what it reads reads the bits too.)
          auto system = query->getParentOfType<SystemOp>();
          if (system.hasContract() && !system.canRead(bitsName)) {
            SmallVector<Attribute> reads;
            if (ArrayAttr declared = system.getReadsAttr())
              llvm::append_range(reads, declared);
            reads.push_back(bitsName);
            system.setReadsAttr(ArrayAttr::get(module.getContext(), reads));
          }
        }
        // (Kept as bits: made by the update of its bits; its rows not
        // kept.)
        if (!clustered) {
          NamedAttrList changed(info);
          Operation *rows =
              symbols.lookup(cast<StringAttr>(info.get("update")));
          changed.set("update", bits);
          info = changed.getDictionary(module.getContext());
          buffer->setAttr("ent.index", info);
          symbols.erase(rows);
          buffer.setCapacity(1);
          auto update = cast<SystemOp>(symbols.lookup(bits));
          // (Where no system changes the field, no entity's value does:
          // the reactive `for`s that find them are left out. Nor where a
          // `gpu` system does, whose changes are not kept on the CPU, nor
          // on many ranks: then its bits are made anew every time.)
          bool byGpu =
              llvm::any_of(module.getOps<SystemOp>(), [&](SystemOp system) {
                return system.getGpu() &&
                       !making.contains(system.getSymNameAttr()) &&
                       writesValues(system, made);
              });
          bool anew = ranks || byGpu;
          if (changing == 0 || anew) {
            SmallVector<Operation *> reactive;
            update.walk([&](QueryOp query) {
              if (query->hasAttr(QueryOp::kTriggersAttr))
                reactive.push_back(query);
            });
            std::string whole = (buffer.getSymName() + "_whole").str();
            if (anew)
              update.walk([&](WriteOp write) {
                if (write.getResource() == whole)
                  reactive.push_back(write);
              });
            for (Operation *op : reactive)
              op->erase();
          }
          // (Nor how many each value has, where nothing reads that,
          // `count`, nor the values that have any, `parts`.)
          auto parts = StringAttr::get(module.getContext(),
                                       buffer.getSymName() + "_parts");
          auto values = StringAttr::get(module.getContext(),
                                        buffer.getSymName() + "_kept");
          // (Where a `gpu` system changes the field, and the program is
          // built for one: its sets made on the GPU, by the update as a
          // `gpu` system; how many each value has then a count of its
          // set's bits where it is asked (`count`), outside the `for`s
          // (which read no set's count), and the values that have any not
          // asked for.)
          if (gpu && byGpu && !ranks)
            makeBitsOnGpu(module, buffer, update, bitsName, values, making,
                          archetypes);
          bool counted = false;
          for (SystemOp system : module.getOps<SystemOp>())
            if (!making.contains(system.getSymNameAttr()))
              for (const Column &column :
                   computeAccess(system, archetypes).reads)
                counted |= std::get<1>(column) == parts ||
                           std::get<1>(column) == values;
          if (!counted) {
            SmallVector<Operation *> counting;
            for (Operation &op :
                 update.getBody().front().without_terminator()) {
              bool counts =
                  op.walk([&](Operation *inner) {
                      auto named =
                          inner->getAttrOfType<FlatSymbolRefAttr>("buffer");
                      return isa<SetCountOp>(inner) ||
                                     (named && (named.getAttr() == parts ||
                                                named.getAttr() == values))
                                 ? WalkResult::interrupt()
                                 : WalkResult::advance();
                    }).wasInterrupted();
              if (counts && op.use_empty())
                counting.push_back(&op);
            }
            for (Operation *op : llvm::reverse(counting))
              op->erase();
          }
          continue;
        }
        // (Else its bits are not kept: what keeps them is left out.)
        symbols.erase(symbols.lookup(bits));
        symbols.erase(symbols.lookup((buffer.getSymName() + "_bits").str()));
        moving = true;
        kept.insert(*held.begin());
        NamedAttrList changed(info);
        changed.set("clustered", UnitAttr::get(module.getContext()));
        info = changed.getDictionary(module.getContext());
        buffer->setAttr("ent.index", info);
        // (Its update moves the rows, which tells each entity its row.)
        auto update = cast<SystemOp>(
            symbols.lookup(cast<StringAttr>(info.get("update"))));
        std::string number = (buffer.getSymName() + "_number").str();
        SmallVector<InvokeOp> numbering;
        update.walk([&](InvokeOp invoke) {
          if (invoke.getCallee() == number)
            numbering.push_back(invoke);
        });
        for (InvokeOp invoke : numbering) {
          SmallVector<Operation *> named;
          for (Value arg : invoke->getOperands())
            if (Operation *def = arg.getDefiningOp())
              named.push_back(def);
          invoke->erase();
          for (Operation *def : named)
            if (def->use_empty())
              def->erase();
        }
        OpBuilder at(update.getBody().front().getTerminator());
        ClusterOp::create(at, buffer.getLoc(),
                          FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
      }
      llvm::stable_sort(indexes, [](auto &a, auto &b) {
        return a.second.get("clustered") && !b.second.get("clustered");
      });
    }

    // An index with a form made on the GPU (a Morton index's) is made so
    // where the program is built for one, all the runs that read it are of
    // gpu systems, and no system on the CPU writes its key or what it
    // copies (spawns do not: those are new entities, which the GPU's form
    // makes anew anyway): its rows are then never wanted on the CPU, nor
    // copied there and back. The other form is left out. (Clustered, its
    // rows moved on the GPU, where the program has no relation, whose
    // trees may keep where their entities are: those are sorted again on
    // the CPU.)
    bool relations = !module.getOps<RelationOp>().empty();
    for (auto &[buffer, info] : indexes) {
      auto onGpu = info.getAs<StringAttr>("gpu_update");
      if (!onGpu)
        continue;
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      bool chosen = gpu && (!info.get("clustered") || !relations);
      bool read = false;
      if (chosen)
        module.walk([&](RunOp run) {
          StringAttr name = run.getSystemAttr().getAttr();
          if (name == kept.update || name == kept.stale || name == kept.moved ||
              name == onGpu)
            return;
          Operation *declared = symbols.lookup(run.getSystem());
          const SystemAccess &access = accessOf(run);
          if (isa<ExternOp>(declared) && access.isOpaque()) {
            chosen = false;
            return;
          }
          auto system = dyn_cast<SystemOp>(declared);
          if (touches(access.reads, kept.readBy)) {
            read = true;
            chosen &= system && system.getGpu();
          }
          if (system && !system.getGpu() && writesValues(system, kept))
            chosen = false;
        });
      Operation *unused = symbols.lookup(chosen && read ? kept.update : onGpu);
      if (chosen && read) {
        NamedAttrList changed(info);
        changed.set("update", onGpu);
        info = changed.getDictionary(module.getContext());
        buffer->setAttr("ent.index", info);
      }
      symbols.erase(unused);
    }

    // A `for` that goes by the field an index is by, over the entities it
    // holds, takes its order from it (QueryOp::kByIndexAttr): the same
    // order, without the sort; and so reads it. Only where the program
    // reads the index itself, which keeps it up to date anyway: made only
    // for such a `for`, the index costs more than the sort it saves where
    // keys change much (made anew, and its rows written and told), and
    // where few change it is made again on one thread where the sort runs
    // on many (bench/RESULTS.md, 2026-10-23). Or where the compiler found
    // it for such `for`s (`inferred`, --ent-infer-indexes), where their
    // key changes less often than they run.
    // (Not in what makes an index: one is made of the entities as they
    // are, not of another index, which may be out of date then.)
    llvm::DenseSet<Attribute> making;
    for (auto [buffer, info] : indexes)
      making.insert(info.get("update"));
    for (auto [buffer, info] : indexes) {
      // (A hash index has no order.)
      if (info.get("hash"))
        continue;
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      bool read = false;
      for (SystemOp system : module.getOps<SystemOp>()) {
        StringAttr name = system.getSymNameAttr();
        if (name == kept.update || name == kept.stale || name == kept.moved)
          continue;
        read |= touches(computeAccess(system, archetypes).reads, kept.readBy);
      }
      if (!read && !info.get("inferred"))
        continue;
      SmallVector<StringAttr> key = strings(info, "key");
      SmallVector<StringAttr> of = strings(info, "of");
      llvm::SmallDenseSet<Attribute> held;
      for (ArchetypeOp archetype : archetypes)
        if (llvm::all_of(of, [&](StringAttr component) {
              return archetype.contains(FlatSymbolRefAttr::get(component)) &&
                     !archetype.isOptional(FlatSymbolRefAttr::get(component));
            }))
          held.insert(archetype.getSymNameAttr());
      module.walk([&](QueryOp query) {
        auto system = query->getParentOfType<SystemOp>();
        if (!system || making.contains(system.getSymNameAttr()) ||
            !query.hasByField() || query.getByIndex() ||
            query.getByComponent().getAttr() != key[0] ||
            query.getByField() != key[1])
          return;
        SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
        if (matched.size() != held.size() ||
            !llvm::all_of(matched, [&](ArchetypeOp archetype) {
              return held.contains(archetype.getSymNameAttr());
            }))
          return;
        if (changesOrderOutside(system, query, query.getByComponent(),
                                query.getByField()))
          return;
        // (Nor where its body spawns: the index does not have those.)
        if (query.getBody()
                .walk([](SpawnOp) { return WalkResult::interrupt(); })
                .wasInterrupted())
          return;
        query->setAttr(QueryOp::kByIndexAttr,
                       FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
      });
    }

    // A `for` whose `where` compares a field with a value the same for
    // all its entities (QueryOp::kKeyEqualsAttr), over the entities a
    // hash index by that field holds, visits only those it says have the
    // value (QueryOp::kViaHashAttr): in their order, as kept in the index
    // (its place), the `where` still asked. Not where the system changes
    // the key or which entities there are, but in the `for` itself and
    // there only of the entity visited (written or sent); nor where the
    // body spawns, adds or removes.
    for (auto [buffer, info] : indexes) {
      if (!info.get("hash"))
        continue;
      SmallVector<StringAttr> key = strings(info, "key");
      SmallVector<StringAttr> of = strings(info, "of");
      StringAttr place;
      for (StringAttr hidden : strings(info, "hidden"))
        if (hidden.getValue().starts_with("place_in_"))
          place = hidden;
      if (!place)
        continue;
      llvm::SmallDenseSet<Attribute> held = holdersOf(archetypes, of);
      module.walk([&](QueryOp query) {
        // (Of one value, which a hash finds.)
        if (!query.getSlicePart("equals") ||
            !answerable(query, key, held, making) ||
            combinesIntoWhatItReads(query))
          return;
        auto component = cast<FlatSymbolRefAttr>(
            query->getAttrOfType<ArrayAttr>(QueryOp::kKeyEqualsAttr)[0]);
        query->setAttr(QueryOp::kViaHashAttr,
                       FlatSymbolRefAttr::get(buffer.getSymNameAttr()));
        query->setAttr(QueryOp::kByAttr,
                       ArrayAttr::get(module.getContext(), {component, place}));
      });
    }
    module.walk([&](QueryOp query) {
      if (!query.getViaHash() && !query.getViaPartition())
        query.dropEquals();
    });

    // The systems that keep the indexes that are not clustered: each
    // writes only fields of its own (where an entity's row is, its
    // place) and its flags, which no other index holds; but it hands a
    // proc the first component to change, which the analysis takes for
    // writing all of it (and so for moving its entities). A clustered
    // one's update moves rows: that one changes the others.
    llvm::DenseSet<Attribute> keepers;
    for (auto [buffer, info] : indexes)
      if (!info.get("clustered"))
        for (StringRef which : {"update", "stale", "moved"})
          keepers.insert(info.get(which));

    OpBuilder builder(module.getContext());
    for (auto [buffer, info] : indexes) {
      Kept kept = keptOf(buffer, info, archetypes, symbols);
      SmallVector<RunOp> runs;
      module.walk([&](RunOp run) { runs.push_back(run); });
      // (One that nothing reads is not made, nor said out of date: a
      // remark says so.)
      bool read = info.get("clustered") || llvm::any_of(runs, [&](RunOp run) {
                    StringAttr system = run.getSystemAttr().getAttr();
                    if (system == kept.update || system == kept.stale ||
                        system == kept.moved || keepers.contains(system))
                      return false;
                    const SystemAccess &access = accessOf(run);
                    return (access.isOpaque() &&
                            isa<ExternOp>(symbols.lookup(run.getSystem()))) ||
                           touches(access.reads, kept.readBy);
                  });
      if (!read && !runs.empty()) {
        if (remarks)
          buffer.emitRemark("index ")
              << buffer.getSymName()
              << " is read by nothing: it is not made (docs/indexes.md)";
        continue;
      }
      for (RunOp run : runs) {
        StringAttr system = run.getSystemAttr().getAttr();
        if (system == kept.update || system == kept.stale ||
            system == kept.moved || keepers.contains(system))
          continue;
        const SystemAccess &access = accessOf(run);
        // (A system with a body that calls out is opaque to the schedule,
        // but its world is what its body does; only an extern system
        // that declares nothing may do anything to it.)
        bool unknown = access.isOpaque() &&
                       isa<ExternOp>(symbols.lookup(run.getSystem()));
        // (A clustered index is made before every run where it is out of
        // date: its archetype's rows are in its order when any system
        // starts.)
        bool reads = unknown || touches(access.reads, kept.readBy) ||
                     info.get("clustered");
        bool moves = unknown || touches(access.writes, kept.holds);
        bool writes = moves || touches(access.writes, kept.values);
        Location loc = run.getLoc();
        if (reads) {
          builder.setInsertionPoint(run);
          auto update = RunOp::create(builder, loc,
                                      FlatSymbolRefAttr::get(kept.update),
                                      ValueRange{});
          Block *condition = builder.createBlock(&update.getCondition());
          builder.setInsertionPointToStart(condition);
          Value fresh = ReadOp::create(builder, loc, builder.getI1Type(),
                                       FlatSymbolRefAttr::get(kept.fresh),
                                       builder.getStringAttr("value"));
          Value yes = arith::ConstantIntOp::create(builder, loc, 1, 1);
          Value stale = arith::XOrIOp::create(builder, loc, fresh, yes);
          // And only where what reads it runs: on its condition too.
          if (!run.getCondition().empty()) {
            IRMapping mapping;
            Block &asked = run.getCondition().front();
            for (Operation &op : asked.without_terminator())
              builder.clone(op, mapping);
            Value runs = mapping.lookupOrDefault(
                asked.getTerminator()->getOperand(0));
            stale = arith::AndIOp::create(builder, loc, stale, runs);
          }
          YieldOp::create(builder, loc, ValueRange{stale});
        }
        if (writes) {
          builder.setInsertionPointAfter(run);
          auto marked = RunOp::create(
              builder, loc,
              FlatSymbolRefAttr::get(moves ? kept.moved : kept.stale),
              ValueRange{});
          // Eager: made again right after what changed it, where it is
          // out of date (not where that run did not run).
          if (info.get("eager")) {
            builder.setInsertionPointAfter(marked);
            auto update =
                RunOp::create(builder, loc, FlatSymbolRefAttr::get(kept.update),
                              ValueRange{});
            Block *condition = builder.createBlock(&update.getCondition());
            builder.setInsertionPointToStart(condition);
            Value fresh = ReadOp::create(builder, loc, builder.getI1Type(),
                                         FlatSymbolRefAttr::get(kept.fresh),
                                         builder.getStringAttr("value"));
            Value yes = arith::ConstantIntOp::create(builder, loc, 1, 1);
            YieldOp::create(
                builder, loc,
                ValueRange{arith::XOrIOp::create(builder, loc, fresh, yes)});
          }
        }
      }
    }
  }
};

/// The indexes a program does not declare that would serve its `for`s
/// (see EntInferIndexes in Passes.td).
struct EntInferIndexes
    : public mlir::ent::impl::EntInferIndexesBase<EntInferIndexes> {
  using EntInferIndexesBase::EntInferIndexesBase;

  /// One index found: what it is by, its kind, its declaration, and the
  /// `for`s it would serve.
  struct Found {
    FlatSymbolRefAttr component;
    StringAttr field;
    bool partition = false;
    /// A sorted index, for `for ... by` the field (else by a value).
    bool sorted = false;
    /// A grid, by where the entities are (`field` the first of those),
    /// for `for`s inside `for`s by how far away they are.
    bool grid = false;
    SmallVector<StringAttr> places;
    std::string cell;
    std::string name;
    /// The `for`s it serves (and of a grid's, the `near` each is to read).
    SmallVector<Operation *> serves;
    llvm::DenseMap<Operation *, std::string> near;
  };

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    SymbolTable symbols(module);
    SmallVector<ArchetypeOp> archetypes(module.getOps<ArchetypeOp>());
    // What the indexes declared are by (those in order apart), and the
    // systems that make them.
    llvm::DenseSet<std::pair<Attribute, Attribute>> declared, ordered;
    llvm::DenseSet<Attribute> making;
    for (BufferOp buffer : module.getOps<BufferOp>()) {
      auto info = buffer->getAttrOfType<DictionaryAttr>("ent.index");
      if (!info)
        continue;
      if (auto key = info.getAs<ArrayAttr>("key"); key && key.size() >= 2) {
        declared.insert({key[0], key[1]});
        if (!info.get("hash"))
          ordered.insert({key[0], key[1]});
      }
      for (NamedAttribute named : info)
        if (auto system = dyn_cast<StringAttr>(named.getValue());
            system && isa_and_nonnull<SystemOp>(symbols.lookup(system)))
          making.insert(system);
    }
    // (An index holds a row for each entity of its component: as many as
    // the component's capacity, or the program's default.)
    bool roomy = module->hasAttr("ent.default_capacity");
    auto room = [&](ComponentOp component) {
      return component && (roomy || component.getCapacityAttr());
    };
    SmallVector<Found> found;
    auto findOrAdd = [&](FlatSymbolRefAttr component, StringAttr field,
                         bool partition, bool sorted = false,
                         bool grid = false) -> Found & {
      for (Found &one : found)
        if (one.component == component && one.field == field &&
            one.sorted == sorted && one.grid == grid)
          return one;
      Found &made = found.emplace_back();
      made.component = component;
      made.field = field;
      made.partition = partition;
      made.sorted = sorted;
      made.grid = grid;
      // (A name of its own: the component's and the field's, or `Near`.)
      std::string base = component.getValue().rsplit('.').second.empty()
                             ? component.getValue().str()
                             : component.getValue().rsplit('.').second.str();
      if (grid) {
        base += "Near";
      } else {
        base += "By";
        base += llvm::toUpper(field.getValue().front());
        base += field.getValue().drop_front().str();
      }
      made.name = base;
      for (int n = 2; symbols.lookup(made.name) ||
                      llvm::any_of(found,
                                   [&](const Found &other) {
                                     return &other != &made &&
                                            other.name == made.name;
                                   });
           ++n)
        made.name = base + std::to_string(n);
      return made;
    };
    module.walk([&](QueryOp query) {
      auto equals = query->getAttrOfType<ArrayAttr>(QueryOp::kKeyEqualsAttr);
      auto system = query->getParentOfType<SystemOp>();
      if (!equals || !system || making.contains(system.getSymNameAttr()))
        return;
      auto component = cast<FlatSymbolRefAttr>(equals[0]);
      auto field = cast<StringAttr>(equals[1]);
      if (declared.contains({component.getAttr(), field}))
        return;
      auto declaredComponent = symbols.lookup<ComponentOp>(component.getAttr());
      if (!room(declaredComponent))
        return;
      Type type = declaredComponent.getFieldType(field.getValue());
      auto whole = dyn_cast_or_null<IntegerType>(type);
      bool partitioned =
          isa_and_nonnull<EnumType>(type) ||
          (whole && (whole.getWidth() == 1 ||
                     (whole.getWidth() == 8 && whole.isUnsigned())));
      SmallVector<StringAttr> key{component.getAttr(), field};
      llvm::SmallDenseSet<Attribute> held =
          holdersOf(archetypes, {component.getAttr()});
      // (As --ent-indexes would answer it: see answerable, overBits, and
      // the hash index's.)
      if (!answerable(query, key, held, making, /*gpu=*/partitioned))
        return;
      if (partitioned) {
        if (!overBits(query))
          return;
      } else if (!query.getSlicePart("equals") ||
                 combinesIntoWhatItReads(query) || system.getGpu() ||
                 !(whole || isa<StringType>(type))) {
        return;
      }
      findOrAdd(component, field, partitioned).serves.push_back(query);
    });

    // A `for ... by` a field, over every entity that has its component,
    // sorts them each time it runs; a sorted index keeps them in order
    // (and the `for` takes its order from it, as --ent-indexes would give
    // it: see the `for`s by an index's key). Made again where the key or
    // the entities change, it pays where more such `for`s run than
    // systems change those (bench/RESULTS.md, 2026-10-23): counted in the
    // program, systems run once (by the world, or by main outside its
    // loops) not counted. (A wrong guess costs time, not what the program
    // does: the index is kept up to date.)
    llvm::DenseSet<Attribute> repeated;
    module.walk([&](CallOp call) {
      if (!isa<MainOp>(call->getParentOp()))
        repeated.insert(call.getScheduleAttr().getAttr());
    });
    llvm::DenseSet<Attribute> often;
    module.walk([&](RunOp run) {
      auto schedule = run->getParentOfType<ScheduleOp>();
      if (!schedule || repeated.contains(schedule.getSymNameAttr()))
        often.insert(run.getSystemAttr().getAttr());
    });
    llvm::DenseMap<std::pair<Attribute, Attribute>, SmallVector<QueryOp>> byKey;
    module.walk([&](QueryOp query) {
      auto system = query->getParentOfType<SystemOp>();
      if (!system || making.contains(system.getSymNameAttr()) ||
          system.getGpu() || !query.hasByField() || query.getByIndex())
        return;
      FlatSymbolRefAttr component = query.getByComponent();
      StringAttr field = query.getByField();
      if (ordered.contains({component.getAttr(), field}))
        return;
      auto declaredComponent = symbols.lookup<ComponentOp>(component.getAttr());
      if (!room(declaredComponent))
        return;
      Type type = declaredComponent.getFieldType(field.getValue());
      if (!isa_and_nonnull<IntegerType, FloatType>(type) || isa<EnumType>(type))
        return;
      llvm::SmallDenseSet<Attribute> held =
          holdersOf(archetypes, {component.getAttr()});
      SmallVector<ArchetypeOp> matched = getMatchedArchetypes(query);
      if (matched.size() != held.size() ||
          !llvm::all_of(matched, [&](ArchetypeOp archetype) {
            return held.contains(archetype.getSymNameAttr());
          }))
        return;
      if (changesOrderOutside(system, query, component, field) ||
          query.getBody()
              .walk([](SpawnOp) { return WalkResult::interrupt(); })
              .wasInterrupted())
        return;
      byKey[{component.getAttr(), field}].push_back(query);
    });
    for (auto &[key, queries] : byKey) {
      auto component = FlatSymbolRefAttr::get(cast<StringAttr>(key.first));
      auto field = cast<StringAttr>(key.second);
      llvm::SmallDenseSet<Attribute> held =
          holdersOf(archetypes, {component.getAttr()});
      StringAttr none = StringAttr::get(module.getContext(), "");
      unsigned changing = 0, going = 0;
      for (SystemOp system : module.getOps<SystemOp>()) {
        if (!often.contains(system.getSymNameAttr()))
          continue;
        SystemAccess access = computeAccess(system, archetypes);
        if (llvm::any_of(held, [&](Attribute archetype) {
              auto name = cast<StringAttr>(archetype);
              return access.writes.contains(
                         {name, component.getAttr(), field}) ||
                     access.writes.contains({name, none, none});
            }))
          ++changing;
      }
      for (QueryOp query : queries)
        going +=
            often.contains(query->getParentOfType<SystemOp>().getSymNameAttr());
      if (going <= changing)
        continue;
      Found &one = findOrAdd(component, field, false, /*sorted=*/true);
      one.serves.assign(queries.begin(), queries.end());
    }

    // A `for` inside a `for` that visits every entity of a component to
    // find those within a distance of the place of the one the `for`
    // around visits (`where (b.x - a.x) * (b.x - a.x) + .. <= r * r`, of
    // two or three fields, `&&` other things) visits the cells around it
    // of a grid of them instead (`near (a.x, a.y) within r in Name`, the
    // `where` still asked): in the grid's order, so only where that order
    // cannot change what it does. Its body only reads, and what it leaves
    // in vars it gathers by `+`, `|`, `&`, `^`, `min` or `max` of whole
    // numbers (a sum of fractions would be rounded otherwise); and the
    // system neither moves the entities nor changes which there are (the
    // grid has them as they were when it started). Of the component's own
    // module; the radius a unique's field or a number, written so.
    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> sources;
    // (What is written at a place of the source: a name, a field of one,
    // or a number.)
    auto writtenAt = [&](Location place) {
      std::string text;
      place->walk([&](Location inner) {
        auto file = dyn_cast<FileLineColLoc>(inner);
        if (!file)
          return WalkResult::advance();
        std::unique_ptr<llvm::MemoryBuffer> &source =
            sources[file.getFilename().getValue()];
        if (!source) {
          auto read =
              llvm::MemoryBuffer::getFile(file.getFilename().getValue());
          if (!read)
            return WalkResult::interrupt();
          source = std::move(*read);
        }
        StringRef rest = source->getBuffer();
        for (unsigned line = 1; line < file.getLine(); ++line)
          rest = rest.split('\n').second;
        text =
            rest.drop_front(file.getColumn() - 1)
                .take_while([](char c) {
                  return llvm::isAlnum(c) || c == '_' || c == '.' || c == ':';
                })
                .str();
        return WalkResult::interrupt();
      });
      return text;
    };
    auto moduleOf = [](StringRef symbol) {
      return symbol.contains('.') ? symbol.rsplit('.').first : StringRef();
    };
    module.walk([&](EachOp each) {
      auto query = each->getParentOfType<QueryOp>();
      auto system = each->getParentOfType<SystemOp>();
      if (!query || !system || making.contains(system.getSymNameAttr()) ||
          system.getGpu() || each.getWith() || each.getWithout() ||
          each.getOld())
        return;
      Block &body = each.getBody().front();
      unsigned vars = each.getInits().size();
      if (vars == 0 ||
          body.getNumArguments() != 1 + vars + (each.getEntity() ? 1 : 0))
        return;
      Value visited = body.getArgument(0);
      auto ref = cast<RefType>(visited.getType());
      FlatSymbolRefAttr component = ref.getComponent();
      if (ref.getVia() ||
          moduleOf(component.getValue()) != moduleOf(system.getSymName()))
        return;
      auto declaredComponent = symbols.lookup<ComponentOp>(component.getAttr());
      if (!room(declaredComponent))
        return;
      ArrayRef<BlockArgument> round = body.getArguments().take_back(vars);
      // (Its `where`: the body in an `if` whose other side leaves the vars.)
      auto end = cast<YieldOp>(body.getTerminator());
      auto where = dyn_cast_or_null<scf::IfOp>(end->getPrevNode());
      if (!where || where.getNumResults() != vars ||
          !llvm::equal(end.getResults(), where.getResults()) ||
          where.getElseRegion().empty() ||
          !llvm::equal(where.elseYield().getOperands(), round))
        return;
      // (Only reads, and what it works out of them.)
      bool reads = true;
      body.walk([&](Operation *op) {
        StringRef dialect = op->getDialect()->getNamespace();
        reads &= dialect == "arith" || dialect == "math" ||
                 isa<scf::IfOp, scf::YieldOp, GetOp, ReadOp, YieldOp>(op) ||
                 llvm::is_contained({"ent.entity", "ent.same", "ent.has"},
                                    op->getName().getStringRef());
      });
      if (!reads)
        return;
      // (Each var gathers by one way that takes its values in any order.)
      llvm::DenseSet<Value> gathered(round.begin(), round.end());
      for (auto [number, var] : llvm::enumerate(round)) {
        StringRef way;
        SmallVector<Value> next{var};
        while (!next.empty() && reads) {
          Value value = next.pop_back_val();
          for (OpOperand &use : value.getUses()) {
            Operation *user = use.getOwner();
            if (user == end) {
              reads &= use.getOperandNumber() == number;
            } else if (auto yield = dyn_cast<scf::YieldOp>(user)) {
              auto branch = dyn_cast<scf::IfOp>(yield->getParentOp());
              reads &= bool(branch);
              if (branch) {
                Value result = branch.getResult(use.getOperandNumber());
                gathered.insert(result);
                next.push_back(result);
              }
            } else if (isa<arith::AddIOp, arith::OrIOp, arith::AndIOp,
                           arith::XOrIOp, arith::MaxSIOp, arith::MinSIOp,
                           arith::MaxUIOp, arith::MinUIOp>(user) &&
                       (way.empty() || way == user->getName().getStringRef()) &&
                       !gathered.contains(
                           user->getOperand(1 - use.getOperandNumber()))) {
              way = user->getName().getStringRef();
              gathered.insert(user->getResult(0));
              next.push_back(user->getResult(0));
            } else {
              reads = false;
            }
          }
        }
      }
      if (!reads)
        return;
      // (How far: a square of differences for each field, from the place
      // of an entity the `for` around binds.)
      Value center;
      SmallVector<std::pair<StringAttr, GetOp>> axes;
      auto difference =
          [&](Value value) -> std::optional<std::pair<StringAttr, GetOp>> {
        Operation *op = value.getDefiningOp();
        if (!isa_and_nonnull<arith::SubFOp, arith::SubIOp>(op))
          return std::nullopt;
        auto mine = op->getOperand(0).getDefiningOp<GetOp>();
        auto theirs = op->getOperand(1).getDefiningOp<GetOp>();
        if (!mine || !theirs)
          return std::nullopt;
        if (theirs.getRef() == visited)
          std::swap(mine, theirs);
        Value at = theirs.getRef();
        if (mine.getRef() != visited || mine.getField() != theirs.getField() ||
            !isa<BlockArgument>(at) ||
            each->isAncestor(
                cast<BlockArgument>(at).getOwner()->getParentOp()) ||
            cast<RefType>(at.getType()).getVia() || (center && at != center))
          return std::nullopt;
        center = at;
        return std::make_pair(mine.getFieldAttr(), theirs);
      };
      std::function<bool(Value)> squares = [&](Value value) {
        Operation *op = value.getDefiningOp();
        if (isa_and_nonnull<arith::AddFOp, arith::AddIOp>(op))
          return squares(op->getOperand(0)) && squares(op->getOperand(1));
        if (!isa_and_nonnull<arith::MulFOp, arith::MulIOp>(op))
          return false;
        auto first = difference(op->getOperand(0));
        auto second = difference(op->getOperand(1));
        if (!first || !second || first->first != second->first ||
            llvm::any_of(
                axes, [&](auto &axis) { return axis.first == first->first; }))
          return false;
        axes.push_back(*first);
        return true;
      };
      SmallVector<Value> conditions{where.getCondition()};
      Value bound;
      while (!conditions.empty() && !bound) {
        Value condition = conditions.pop_back_val();
        Operation *op = condition.getDefiningOp();
        if (auto both = dyn_cast_or_null<arith::AndIOp>(op)) {
          conditions.append({both.getLhs(), both.getRhs()});
          continue;
        }
        Value near, far;
        if (auto compare = dyn_cast_or_null<arith::CmpFOp>(op)) {
          using P = arith::CmpFPredicate;
          P predicate = compare.getPredicate();
          if (llvm::is_contained({P::OLE, P::OLT, P::ULE, P::ULT}, predicate))
            std::tie(near, far) =
                std::make_pair(compare.getLhs(), compare.getRhs());
          else if (llvm::is_contained({P::OGE, P::OGT, P::UGE, P::UGT},
                                      predicate))
            std::tie(near, far) =
                std::make_pair(compare.getRhs(), compare.getLhs());
        } else if (auto compare = dyn_cast_or_null<arith::CmpIOp>(op)) {
          using P = arith::CmpIPredicate;
          P predicate = compare.getPredicate();
          if (llvm::is_contained({P::sle, P::slt}, predicate))
            std::tie(near, far) =
                std::make_pair(compare.getLhs(), compare.getRhs());
          else if (llvm::is_contained({P::sge, P::sgt}, predicate))
            std::tie(near, far) =
                std::make_pair(compare.getRhs(), compare.getLhs());
        }
        center = nullptr;
        axes.clear();
        if (near && squares(near) && (axes.size() == 2 || axes.size() == 3))
          bound = far;
      }
      if (!bound)
        return;
      // (Places of one type, fractions or whole numbers of 64 bits.)
      Type type = declaredComponent.getFieldType(axes[0].first.getValue());
      if (!(type.isF32() || type.isF64() || type.isInteger(64)) ||
          llvm::any_of(axes, [&](auto &axis) {
            return declaredComponent.getFieldType(axis.first.getValue()) !=
                   type;
          }))
        return;
      // (The radius: what is squared, as written.)
      std::string radius;
      Operation *squared = bound.getDefiningOp();
      if (isa_and_nonnull<arith::MulFOp, arith::MulIOp>(squared)) {
        Operation *one = squared->getOperand(0).getDefiningOp();
        Operation *other = squared->getOperand(1).getDefiningOp();
        auto read = dyn_cast_or_null<ReadOp>(one);
        auto otherRead = dyn_cast_or_null<ReadOp>(other);
        auto number = dyn_cast_or_null<arith::ConstantOp>(one);
        if ((read && otherRead &&
             read.getResourceAttr() == otherRead.getResourceAttr() &&
             read.getField() == otherRead.getField()) ||
            (number && one && other &&
             OperationEquivalence::isEquivalentTo(
                 one, other, OperationEquivalence::Flags::IgnoreLocations)))
          radius = writtenAt(one->getLoc());
      }
      if (radius.empty())
        return;
      // (The place, as the `for` around names it.)
      SmallVector<std::string> place;
      for (auto &[field, get] : axes) {
        std::string text = writtenAt(get.getLoc());
        if (!StringRef(text).ends_with(("." + field.getValue()).str()))
          return;
        place.push_back(text);
      }
      // (Not where the system moves them or changes which there are.)
      StringAttr none = StringAttr::get(module.getContext(), "");
      SystemAccess access = computeAccess(system, archetypes);
      auto centerComponent = cast<RefType>(center.getType()).getComponent();
      for (FlatSymbolRefAttr moved : {component, centerComponent})
        for (Attribute archetype : holdersOf(archetypes, {moved.getAttr()})) {
          auto name = cast<StringAttr>(archetype);
          if (access.writes.contains({name, none, none}) ||
              llvm::any_of(axes, [&](auto &axis) {
                return access.writes.contains(
                    {name, moved.getAttr(), axis.first});
              }))
            return;
        }
      SmallVector<StringAttr> places;
      for (auto &axis : axes)
        places.push_back(axis.first);
      Found &one = findOrAdd(component, places[0], false, false, /*grid=*/true);
      if (one.places.empty()) {
        one.places = places;
        one.cell = radius;
      } else if (one.places != places) {
        return;
      }
      // (A little farther than the radius for fractions: the grid works
      // out how far as it does, and the `where` is still asked.)
      std::string within = radius;
      if (isa<FloatType>(type))
        within += " * 1.000001";
      one.serves.push_back(each);
      one.near[each] = "near (" + llvm::join(place, ", ") + ") within " +
                       within + " in " + one.name;
    });

    // (Of a trace: how long each `for`'s loops took, by its place, and
    // the run; those that took less than `worth` of it are left.)
    llvm::StringMap<double> took;
    double run = 0;
    if (!times.empty()) {
      auto read = llvm::MemoryBuffer::getFile(times);
      llvm::Expected<llvm::json::Value> parsed =
          read ? llvm::json::parse((*read)->getBuffer())
               : llvm::Expected<llvm::json::Value>(
                     llvm::errorCodeToError(read.getError()));
      if (!parsed) {
        module.emitError("cannot read the trace's times ")
            << times << ": " << llvm::toString(parsed.takeError());
        return signalPassFailure();
      }
      if (const llvm::json::Object *object = parsed->getAsObject()) {
        run = object->getNumber("run").value_or(0);
        if (const llvm::json::Object *places = object->getObject("places"))
          for (const auto &[place, value] : *places)
            took[place.str()] = value.getAsNumber().value_or(0);
      }
    }
    // (A `for` inside a `for` is timed as the `for` around.)
    auto placeOf = [](Operation *served) {
      if (auto each = dyn_cast<EachOp>(served))
        served = each->getParentOfType<QueryOp>();
      std::string place;
      served->getLoc()->walk([&](Location inner) {
        auto file = dyn_cast<FileLineColLoc>(inner);
        if (!file)
          return WalkResult::advance();
        place = (llvm::sys::path::filename(file.getFilename().getValue()) +
                 ":" + Twine(file.getLine()) + ":" + Twine(file.getColumn()))
                    .str();
        return WalkResult::interrupt();
      });
      return place;
    };
    if (!times.empty()) {
      for (Found &one : found)
        llvm::erase_if(one.serves, [&](Operation *served) {
          return took.lookup(placeOf(served)) < worth * run;
        });
      llvm::erase_if(found,
                     [](const Found &one) { return one.serves.empty(); });
    }

    std::string text;
    llvm::raw_string_ostream lines(text);
    for (Found &one : found) {
      // (A component of another module by its name there: `circuit::And`.)
      SmallVector<StringRef> parts;
      one.component.getValue().split(parts, '.');
      std::string component = llvm::join(parts, "::");
      std::string declaration = "index " + one.name + " of " + component;
      if (one.grid) {
        SmallVector<StringRef> places;
        for (StringAttr place : one.places)
          places.push_back(place.getValue());
        declaration +=
            " grid (" + llvm::join(places, ", ") + ") cell " + one.cell;
      } else {
        declaration += " by " + one.field.getValue().str() +
                       (one.sorted      ? ""
                        : one.partition ? " partition"
                                        : " hash");
      }
      lines << declaration << "\n";
      // (And where a grid's `near`s are read: the place of each `for`.)
      for (Operation *served : one.serves)
        if (auto place = dyn_cast<FileLineColLoc>(served->getLoc());
            place && one.grid)
          lines << "at " << place.getFilename().getValue() << ":"
                << place.getLine() << ":" << place.getColumn() << "\t"
                << one.near[served] << "\n";
      if (!remarks)
        continue;
      for (Operation *query : one.serves) {
        InFlightDiagnostic remark =
            one.sorted ? query->emitRemark("sorts the entities by ")
            : one.grid
                ? query->emitRemark("visits every entity to find those within ")
                : query->emitRemark("visits every entity to find those whose ");
        if (one.sorted)
          remark << one.field.getValue()
                 << " each time it runs, and more such 'for's run than "
                    "systems change them";
        else if (one.grid)
          remark << one.cell << " of a place";
        else
          remark << one.field.getValue()
                 << (one.partition ? " is of some values" : " is one value");
        if (!times.empty()) {
          std::string spent;
          llvm::raw_string_ostream said(spent);
          said << " ("
               << llvm::format("%.1f", took.lookup(placeOf(query)) / 1000.0)
               << " ms of the " << llvm::format("%.1f", run / 1000.0)
               << " traced)";
          remark << spent;
        }
        remark << (one.sorted ? "; an index would keep them in order: '"
                              : "; an index would find them: '")
               << declaration << "'";
        if (one.grid)
          remark << ", and '" << one.near[query] << "'";
      }
    }
    if (out.empty())
      return;
    std::error_code error;
    llvm::raw_fd_ostream file(out, error, llvm::sys::fs::OF_Text);
    if (error) {
      module.emitError("cannot write the indexes found to ")
          << out << ": " << error.message();
      return signalPassFailure();
    }
    file << text;
  }
};

} // namespace
