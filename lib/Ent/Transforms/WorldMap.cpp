// The world map of a program (--ent-world-map): what its world is made of
// and what each system does to it, written as lines of tab-separated
// fields for tools to read (`ent map` shows them). Its records, each a
// line, the kind first:
//
//   ent-map 1
//   component  name  fields  file:line  apart(0/1)
//   resource   name  fields  file:line
//   archetype  index  name  capacity  inferred  required  optional  label
//   system     name  file:line  gpu  opaque-at (or "")
//   reads      system  columns          writes  system  columns
//   query      system  file:line  archetypes (indexes)
//   change     system  file:line  add|remove|spawn|despawn  component
//              archetype  presence|overwrite|nothing|move|none  target
//   schedule   name  file:line
//   run        schedule  position  system  stage  waits-for  why
//   call       schedule  loop-depth
//   row, column, cell: systems by what they touch (see below)
//   lens       components whose presence states tell ("*": all)
//   state      index  archetype  present optional components
//   flow       step  schedule  position  system  from  to  kind  file:line
//
// Lists inside a field are joined with ",". Columns are as ent-print-access
// says them. `stage` is the stage ent-schedule would give a run of a
// schedule (runs of one stage commute); `waits-for` is the earlier run it
// conflicts with, `why` the conflict.
//
// The states and flows are the entities' lifecycle, as far as the program
// says it: a state is an archetype with which of its optional components
// of the lens are present (the others, which a query may test, may be
// present or not: ent-map's `lens` option, which names them; otherwise the
// program's own components, of which it adds or removes some); each spawn makes a state (from "born", -1), each add and
// remove in a query takes the states of the entities it visits to others,
// each despawn takes them to "gone" (-2). The calls of `main` are followed
// in order, those in loops until no new state comes (each loop's body as
// many times as it takes); a flow is a step (a run that changes states),
// from a state to another. What may happen is in it: a change under a
// condition, a run that does not run every time, a query that may visit no
// entity. A state is entities of one kind in one condition, whichever
// those are at run time.
#include "Ent/Access.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>

namespace mlir::ent {
#define GEN_PASS_DEF_ENTWORLDMAP
#include "Ent/Passes.h.inc"
} // namespace mlir::ent

using namespace mlir;
using namespace mlir::ent;

namespace {

/// The .ent file and line `loc` is of (the first it holds), or "".
std::string placeOf(Location loc) {
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

/// A name with tabs and line breaks as spaces (a field of a record).
std::string field(StringRef text) {
  std::string out = text.str();
  for (char &c : out)
    if (c == '\t' || c == '\n' || c == '\r')
      c = ' ';
  return out;
}

std::string joined(ArrayRef<std::string> parts) {
  std::string out;
  llvm::interleave(parts, [&](const std::string &p) { out += p; },
                   [&] { out += ","; });
  return out;
}

std::string names(ArrayRef<Attribute> refs) {
  SmallVector<std::string> parts;
  for (Attribute ref : refs)
    parts.push_back(field(cast<FlatSymbolRefAttr>(ref).getValue()));
  return joined(parts);
}

/// The part of a symbol after its module's ("collection.Entry": "Entry").
StringRef shortName(StringRef name) {
  size_t dot = name.rfind('.');
  return dot == StringRef::npos ? name : name.substr(dot + 1);
}

/// An entity's state: its archetype, and which optional components of it
/// are present (by their place in the archetype's optional list).
struct State {
  unsigned archetype;
  std::vector<bool> present;
  bool operator<(const State &other) const {
    return std::tie(archetype, present) <
           std::tie(other.archetype, other.present);
  }
};

constexpr int born = -1, gone = -2;
constexpr size_t mostStates = 20000;

struct Flow {
  int from, to;
  std::string kind, place;
  bool operator<(const Flow &other) const {
    return std::tie(from, to, kind, place) <
           std::tie(other.from, other.to, other.kind, other.place);
  }
};

struct Mapper {
  ModuleOp module;
  SymbolTable symbols;
  SmallVector<ArchetypeOp> archetypes;
  llvm::DenseMap<Operation *, unsigned> archetypeIndex;
  std::map<State, int> stateIndex;
  std::vector<State> states;
  bool tooMany = false;
  /// The components whose presence the states tell (the lens), or all.
  std::set<std::string> lens;
  bool everyComponent = false;

  bool seen(StringRef component) {
    return everyComponent || lens.count(component.str());
  }

  Mapper(ModuleOp module)
      : module(module), symbols(module),
        archetypes(module.getOps<ArchetypeOp>()) {
    for (auto [i, archetype] : llvm::enumerate(archetypes))
      archetypeIndex[archetype] = i;
  }

  ArrayRef<Attribute> optionals(ArchetypeOp archetype) {
    if (auto optional = archetype.getOptionalAttr())
      return optional.getValue();
    return {};
  }

  int optionalPlace(ArchetypeOp archetype, StringRef component) {
    for (auto [i, ref] : llvm::enumerate(optionals(archetype)))
      if (cast<FlatSymbolRefAttr>(ref).getValue() == component)
        return i;
    return -1;
  }

  /// Whether an entity in `state` has `component`; nothing where the
  /// state does not tell (optional, not in the lens: it may or not).
  std::optional<bool> has(const State &state, FlatSymbolRefAttr component) {
    ArchetypeOp archetype = archetypes[state.archetype];
    int place = optionalPlace(archetype, component.getValue());
    if (place >= 0) {
      if (!seen(component.getValue()))
        return std::nullopt;
      return bool(state.present[place]);
    }
    return archetype.contains(component);
  }

  int stateOf(const State &state) {
    auto [it, inserted] = stateIndex.try_emplace(state, states.size());
    if (inserted) {
      if (states.size() >= mostStates) {
        tooMany = true;
        stateIndex.erase(it);
        return -3;
      }
      states.push_back(state);
    }
    return it->second;
  }

  /// The state a spawn makes.
  std::optional<State> spawned(SpawnOp spawn) {
    auto into = spawn.getArchetypeAttr();
    if (!into)
      return std::nullopt;
    auto archetype = symbols.lookup<ArchetypeOp>(into.getValue());
    if (!archetype)
      return std::nullopt;
    State state{archetypeIndex[archetype],
                std::vector<bool>(optionals(archetype).size())};
    for (auto [i, ref] : llvm::enumerate(optionals(archetype))) {
      auto c = cast<FlatSymbolRefAttr>(ref);
      state.present[i] = seen(c.getValue()) && spawn.startsWith(c);
    }
    return state;
  }

  /// Whether `query` visits entities in `state`.
  bool visits(QueryOp query, const State &state) {
    ArchetypeOp archetype = archetypes[state.archetype];
    if (!matches(query, archetype))
      return false;
    PresenceTest test = getPresenceTest(query, archetype);
    for (FlatSymbolRefAttr c : test.present)
      if (has(state, c) == std::optional<bool>(false))
        return false;
    for (FlatSymbolRefAttr c : test.absent)
      if (has(state, c) == std::optional<bool>(true))
        return false;
    for (auto &group : test.anyPresent)
      if (llvm::all_of(group, [&](FlatSymbolRefAttr c) {
            return has(state, c) == std::optional<bool>(false);
          }))
        return false;
    return true;
  }

  /// The state after adding or removing `component` (nothing where it
  /// does not change: overwritten, not held; or no archetype to move to).
  std::optional<State> changed(const State &state, FlatSymbolRefAttr component,
                               bool add) {
    ArchetypeOp archetype = archetypes[state.archetype];
    ComponentChange change = classifyChange(archetype, component, add);
    if (change.kind == ComponentChange::Presence) {
      if (!seen(component.getValue()))
        return std::nullopt;
      State next = state;
      next.present[optionalPlace(archetype, component.getValue())] = add;
      if (next.present == state.present)
        return std::nullopt;
      return next;
    }
    if (change.kind != ComponentChange::Move || !change.target)
      return std::nullopt;
    // (What it had optionally it keeps where the target holds it so.)
    ArchetypeOp target = change.target;
    State next{archetypeIndex[target],
               std::vector<bool>(optionals(target).size())};
    for (auto [i, ref] : llvm::enumerate(optionals(target))) {
      auto c = cast<FlatSymbolRefAttr>(ref);
      next.present[i] = seen(c.getValue()) &&
                        (c == component ? add : has(state, c).value_or(false));
    }
    return next;
  }

  /// The ops that act on the entity `query` visits: its own adds, removes
  /// and despawns, not those of queries inside it, in program order.
  SmallVector<Operation *> actions(QueryOp query) {
    SmallVector<Operation *> found;
    query.getBody().walk([&](Operation *op) {
      if (!isa<AddOp, RemoveOp, DespawnOp>(op))
        return;
      if (op->getParentOfType<QueryOp>() == query)
        found.push_back(op);
    });
    return found;
  }

  /// What a run of `system` does to entities in `known` states: the flows
  /// (new states added to the map as found).
  std::set<Flow> flows(SystemOp system, const std::vector<int> &known) {
    std::set<Flow> out;
    system.getBody().walk([&](SpawnOp spawn) {
      if (auto state = spawned(spawn)) {
        int to = stateOf(*state);
        if (to >= 0)
          out.insert({born, to, "spawn", placeOf(spawn.getLoc())});
      }
    });
    system.getBody().walk([&](QueryOp query) {
      SmallVector<Operation *> acts = actions(query);
      if (acts.empty())
        return;
      for (int from : known) {
        if (!visits(query, states[from]))
          continue;
        // Each change may happen or not (under a condition), one after
        // another: every state reached, with the changes that reach it,
        // is a flow from the state the entity had when the run began (so
        // "remove Lit, add Done" where both come one after the other).
        struct Reached {
          int state;
          std::string kinds, places;
        };
        std::vector<Reached> reached = {{from, "", ""}};
        for (Operation *act : acts) {
          std::string place = placeOf(act->getLoc());
          auto then = [](const std::string &before, const std::string &more) {
            return before.empty() ? more : before + ", " + more;
          };
          if (isa<DespawnOp>(act)) {
            for (auto &r : reached)
              out.insert({from, gone, then(r.kinds, "despawn"),
                          then(r.places, place)});
            continue;
          }
          bool add = isa<AddOp>(act);
          auto component = add ? cast<AddOp>(act).getComponentAttr()
                               : cast<RemoveOp>(act).getComponentAttr();
          std::string kind = (add ? "add " : "remove ") +
                             shortName(component.getValue()).str();
          std::vector<Reached> more;
          for (auto &r : reached) {
            auto next = changed(states[r.state], component, add);
            if (!next)
              continue;
            int to = stateOf(*next);
            if (to < 0)
              continue;
            more.push_back({to, then(r.kinds, kind), then(r.places, place)});
          }
          for (auto &m : more)
            if (llvm::none_of(reached, [&](auto &r) { return r.state == m.state; }))
              reached.push_back(m);
        }
        for (auto &r : reached)
          if (r.state != from)
            out.insert({from, r.state, r.kinds, r.places});
      }
    });
    return out;
  }
};

struct Step {
  ScheduleOp schedule;
  RunOp run;
  unsigned position;
  unsigned depth;
};

struct EntWorldMap : public mlir::ent::impl::EntWorldMapBase<EntWorldMap> {
  using EntWorldMapBase::EntWorldMapBase;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    if (failed(inferArchetypes(module)))
      return signalPassFailure();
    std::error_code error;
    std::string path = out.empty() ? std::string("-") : std::string(out);
    llvm::raw_fd_ostream os(path, error,
                            llvm::sys::fs::OF_Text);
    if (error) {
      module.emitError() << "cannot write the world map to '" << out
                         << "': " << error.message();
      return signalPassFailure();
    }
    Mapper map(module);
    os << "ent-map\t1\n";

    // What the world is made of.
    // (An archetype's components are all it holds; those it requires are
    // the ones not optional.)
    auto required = [&](ArchetypeOp archetype) {
      SmallVector<Attribute> out;
      for (Attribute ref : archetype.getComponents())
        if (!archetype.isOptional(cast<FlatSymbolRefAttr>(ref)))
          out.push_back(ref);
      return out;
    };
    llvm::StringMap<unsigned> holders;
    for (ArchetypeOp archetype : map.archetypes)
      for (Attribute ref : required(archetype))
        holders[cast<FlatSymbolRefAttr>(ref).getValue()]++;
    for (Operation &op : module.getOps()) {
      if (auto component = dyn_cast<ComponentOp>(op)) {
        SmallVector<std::string> fields;
        for (Attribute name : component.getFieldNames())
          fields.push_back(field(cast<StringAttr>(name).getValue()));
        os << "component\t" << field(component.getSymName()) << "\t"
           << joined(fields) << "\t" << placeOf(op.getLoc()) << "\t"
           << (component.getApart() ? 1 : 0) << "\n";
      } else if (auto resource = dyn_cast<ResourceOp>(op)) {
        SmallVector<std::string> fields;
        for (Attribute name : resource.getFieldNames())
          fields.push_back(field(cast<StringAttr>(name).getValue()));
        os << "resource\t" << field(resource.getSymName()) << "\t"
           << joined(fields) << "\t" << placeOf(op.getLoc()) << "\n";
      }
    }
    for (auto [i, archetype] : llvm::enumerate(map.archetypes)) {
      // (Its label: the components it requires that the fewest archetypes
      // hold, the ones that tell it from the others.)
      unsigned fewest = ~0u;
      for (Attribute ref : required(archetype))
        fewest = std::min(fewest, holders[cast<FlatSymbolRefAttr>(ref).getValue()]);
      SmallVector<std::string> label;
      for (Attribute ref : required(archetype)) {
        StringRef name = cast<FlatSymbolRefAttr>(ref).getValue();
        if (holders[name] == fewest)
          label.push_back(field(shortName(name)));
      }
      os << "archetype\t" << i << "\t" << field(archetype.getSymName()) << "\t"
         << archetype.getCapacity() << "\t" << (archetype.getInferred() ? 1 : 0)
         << "\t" << names(required(archetype)) << "\t"
         << names(map.optionals(archetype)) << "\t" << joined(label) << "\n";
    }

    // What each system does.
    llvm::DenseMap<Operation *, SystemAccess> accesses;
    for (Operation &op : module.getOps()) {
      if (!isa<SystemOp, ExternOp>(op))
        continue;
      SystemAccess access = computeAccess(&op, map.archetypes);
      accesses[&op] = access;
      StringRef name = SymbolTable::getSymbolName(&op).getValue();
      auto system = dyn_cast<SystemOp>(op);
      os << "system\t" << field(name) << "\t" << placeOf(op.getLoc()) << "\t"
         << (system && system.getGpu() ? 1 : 0) << "\t"
         << (access.isOpaque() ? placeOf(access.opaqueOp->getLoc()) : "")
         << "\n";
      SmallVector<std::string> reads, writes;
      for (const Column &c : access.reads)
        reads.push_back(field(formatColumn(c)));
      for (const Column &c : access.writes)
        writes.push_back(field(formatColumn(c)));
      os << "reads\t" << field(name) << "\t" << joined(reads) << "\n";
      os << "writes\t" << field(name) << "\t" << joined(writes) << "\n";
      if (!system)
        continue;
      system.getBody().walk([&](Operation *inner) {
        if (auto query = dyn_cast<QueryOp>(inner)) {
          SmallVector<std::string> matched;
          for (ArchetypeOp a : getMatchedArchetypes(query))
            matched.push_back(std::to_string(map.archetypeIndex[a]));
          os << "query\t" << field(name) << "\t" << placeOf(query.getLoc())
             << "\t" << joined(matched) << "\n";
        } else if (auto spawn = dyn_cast<SpawnOp>(inner)) {
          auto into = spawn.getArchetypeAttr();
          os << "change\t" << field(name) << "\t" << placeOf(spawn.getLoc())
             << "\tspawn\t\t" << (into ? field(into.getValue()) : "")
             << "\t\t\n";
        } else if (isa<AddOp, RemoveOp, DespawnOp>(inner)) {
          auto query = inner->getParentOfType<QueryOp>();
          if (!query)
            return;
          bool add = isa<AddOp>(inner);
          FlatSymbolRefAttr component =
              add ? cast<AddOp>(inner).getComponentAttr()
                  : isa<RemoveOp>(inner) ? cast<RemoveOp>(inner).getComponentAttr()
                                         : FlatSymbolRefAttr();
          for (ArchetypeOp a : getMatchedArchetypes(query)) {
            os << "change\t" << field(name) << "\t" << placeOf(inner->getLoc())
               << "\t";
            if (!component) {
              os << "despawn\t\t" << field(a.getSymName()) << "\t\t\n";
              continue;
            }
            ComponentChange change = classifyChange(a, component, add);
            static const char *kinds[] = {"presence", "overwrite", "nothing",
                                          "move", "none"};
            os << (add ? "add" : "remove") << "\t"
               << field(component.getValue()) << "\t" << field(a.getSymName())
               << "\t" << kinds[change.kind] << "\t"
               << (change.target ? field(change.target.getSymName()) : "")
               << "\n";
          }
        }
      });
    }

    // The schedules, staged as ent-schedule would (barriers aside).
    struct RunRow {
      ScheduleOp schedule;
      RunOp run;
      unsigned position, stage;
      std::string waits, why;
    };
    llvm::DenseMap<Operation *, SmallVector<RunRow>> scheduleRows;
    for (ScheduleOp schedule : module.getOps<ScheduleOp>()) {
      os << "schedule\t" << field(schedule.getSymName()) << "\t"
         << placeOf(schedule.getLoc()) << "\n";
      SmallVector<RunOp> runs;
      schedule.getBody().walk([&](RunOp run) { runs.push_back(run); });
      SmallVector<SystemAccess> access;
      for (RunOp run : runs) {
        SystemAccess a = accesses.lookup(map.symbols.lookup(run.getSystem()));
        addConditionReads(run.getCondition(), a);
        access.push_back(a);
      }
      SmallVector<unsigned> level(runs.size(), 0);
      for (unsigned j = 0; j < runs.size(); ++j) {
        std::string reason, waits;
        for (unsigned i = 0; i < j; ++i) {
          if (level[i] + 1 <= level[j])
            continue;
          auto conflict =
              access[j].conflictWith(access[i], runs[i].getSystem());
          if (!conflict)
            continue;
          level[j] = level[i] + 1;
          reason = *conflict;
          waits = runs[i].getSystem().str();
        }
        os << "run\t" << field(schedule.getSymName()) << "\t" << j << "\t"
           << field(runs[j].getSystem()) << "\t" << level[j] << "\t"
           << field(waits) << "\t" << field(reason) << "\n";
        scheduleRows[schedule].push_back(
            {schedule, runs[j], j, level[j], waits, reason});
      }
    }

    // Systems by what they touch: a row for each run, the schedules in the
    // order `main` first calls them, then the others; a column for each
    // component, unique, relation and buffer, and the entities themselves
    // (their tables, counts and ids) and the ticks of reactive queries, in
    // the order of the first row that touches it; a cell where a row reads
    // (r), writes (w) or both (rw) one, with which parts: fields, "?"
    // presence, "+" added, "-" removed, "f@" changed, "~" connected,
    // "edges", "count", "id", "table", in any archetype.
    //   row     index  schedule  position  system  stage  waits  why
    //           file:line  opaque-at  gpu  main (1: a schedule `main`
    //           calls; the others' runs are in those too, where they
    //           are called from them, but stand here on their own)
    //   column  index  kind  name
    //   cell    row  column  r|w|rw  parts read  parts written
    SmallVector<ScheduleOp> order;
    for (MainOp main : module.getOps<MainOp>())
      main.getBody().walk([&](CallOp call) {
        if (auto schedule = map.symbols.lookup<ScheduleOp>(call.getSchedule()))
          if (!llvm::is_contained(order, schedule))
            order.push_back(schedule);
      });
    unsigned called = order.size();
    for (ScheduleOp schedule : module.getOps<ScheduleOp>())
      if (!llvm::is_contained(order, schedule))
        order.push_back(schedule);
    // (What a column is called, and of what kind; the part a column of
    // the access analysis is of it.)
    auto columnOf = [&](const Column &c) -> std::tuple<std::string, std::string, std::string> {
      auto [archetype, component, part] = c;
      StringRef name = component ? component.getValue() : StringRef();
      if (name.empty()) {
        if (!archetype && part && part.getValue() == "ticks")
          return {"world", "ticks", "ticks"};
        if (!archetype)
          return {"world", "entities", "table"};
        return {"world", "entities", part.getValue().empty() ? "count" : "id"};
      }
      std::string kind = "component";
      Operation *op = map.symbols.lookup(name);
      if (isa_and_nonnull<ResourceOp>(op))
        kind = "unique";
      else if (isa_and_nonnull<RelationOp>(op))
        kind = "relation";
      else if (isa_and_nonnull<BufferOp>(op))
        kind = "buffer";
      std::string what = part ? part.getValue().str() : "";
      if (what.empty())
        what = archetype ? "?" : "edges";
      return {kind, name.str(), what};
    };
    std::vector<std::pair<std::string, std::string>> columns;
    std::map<std::string, unsigned> columnIndex;
    struct Cell {
      std::set<std::string> reads, writes;
    };
    std::vector<std::map<unsigned, Cell>> cells;
    unsigned row = 0;
    for (auto [nth, schedule] : llvm::enumerate(order)) {
      for (RunRow &r : scheduleRows[schedule]) {
        Operation *system = map.symbols.lookup(r.run.getSystem());
        SystemAccess access = accesses.lookup(system);
        addConditionReads(r.run.getCondition(), access);
        auto system_op = dyn_cast_or_null<SystemOp>(system);
        os << "row\t" << row << "\t" << field(schedule.getSymName()) << "\t"
           << r.position << "\t" << field(r.run.getSystem()) << "\t" << r.stage
           << "\t" << field(r.waits) << "\t" << field(r.why) << "\t"
           << (system ? placeOf(system->getLoc()) : "") << "\t"
           << (access.isOpaque() ? placeOf(access.opaqueOp->getLoc()) : "")
           << "\t" << (system_op && system_op.getGpu() ? 1 : 0) << "\t"
           << (nth < called ? 1 : 0) << "\n";
        cells.emplace_back();
        auto note = [&](const Column &c, bool write) {
          auto [kind, name, part] = columnOf(c);
          auto [it, inserted] = columnIndex.try_emplace(name, columns.size());
          if (inserted)
            columns.push_back({kind, name});
          Cell &cell = cells.back()[it->second];
          (write ? cell.writes : cell.reads).insert(part);
        };
        for (const Column &c : access.reads)
          note(c, false);
        for (const Column &c : access.writes)
          note(c, true);
        ++row;
      }
    }
    for (auto [i, column] : llvm::enumerate(columns))
      os << "column\t" << i << "\t" << column.first << "\t"
         << field(column.second) << "\n";
    for (auto [i, rowCells] : llvm::enumerate(cells))
      for (auto &[c, cell] : rowCells) {
        SmallVector<std::string> reads(cell.reads.begin(), cell.reads.end());
        SmallVector<std::string> writes(cell.writes.begin(), cell.writes.end());
        os << "cell\t" << i << "\t" << c << "\t"
           << (cell.writes.empty() ? "r" : cell.reads.empty() ? "w" : "rw")
           << "\t" << field(joined(reads)) << "\t" << field(joined(writes))
           << "\n";
      }

    // The lens: the components named, or all ("*"); where none are, those
    // the program declares itself (not in the `library` folders) that a
    // query adds or removes; where it has none, all.
    if (lens == "*") {
      map.everyComponent = true;
    } else if (!lens.empty()) {
      for (StringRef name : llvm::split(StringRef(lens), ','))
        if (!name.trim().empty())
          map.lens.insert(name.trim().str());
    } else {
      SmallVector<StringRef> folders;
      StringRef(library).split(folders, ',', -1, false);
      auto declaredIn = [&](Operation *op) {
        std::string file;
        op->getLoc()->walk([&](Location inner) {
          if (auto at = dyn_cast<FileLineColLoc>(inner)) {
            file = at.getFilename().str();
            return WalkResult::interrupt();
          }
          return WalkResult::advance();
        });
        return file;
      };
      module.walk([&](Operation *op) {
        FlatSymbolRefAttr c;
        if (auto add = dyn_cast<AddOp>(op))
          c = add.getComponentAttr();
        else if (auto remove = dyn_cast<RemoveOp>(op))
          c = remove.getComponentAttr();
        if (!c)
          return;
        auto component = map.symbols.lookup<ComponentOp>(c.getValue());
        if (!component)
          return;
        std::string file = declaredIn(component);
        if (llvm::any_of(folders, [&](StringRef folder) {
              return StringRef(file).starts_with(folder);
            }))
          return;
        map.lens.insert(c.getValue().str());
      });
      if (map.lens.empty())
        map.everyComponent = true;
    }
    {
      SmallVector<std::string> shown(map.lens.begin(), map.lens.end());
      os << "lens\t" << (map.everyComponent ? "*" : joined(shown)) << "\n";
    }

    // The order schedules run in, and the entities' lifecycle along it.
    SmallVector<std::pair<ScheduleOp, unsigned>> calls;
    for (MainOp main : module.getOps<MainOp>())
      main.getBody().walk([&](CallOp call) {
        unsigned depth = 0;
        for (Operation *up = call->getParentOp(); !isa<MainOp>(up);
             up = up->getParentOp())
          depth += isa<LoopOp>(up);
        if (auto schedule = map.symbols.lookup<ScheduleOp>(call.getSchedule()))
          calls.push_back({schedule, depth});
      });
    SmallVector<Step> steps;
    for (auto [schedule, depth] : calls) {
      os << "call\t" << field(schedule.getSymName()) << "\t" << depth << "\n";
      unsigned position = 0;
      schedule.getBody().walk([&](RunOp run) {
        steps.push_back({schedule, run, position++, depth});
      });
    }
    // (Every state found so far goes into every step: the loops repeat,
    // and what is reached anywhere in one may be there at its start.)
    std::vector<std::set<Flow>> found(steps.size());
    for (unsigned round = 0; round < 64; ++round) {
      size_t before = map.states.size();
      for (auto [i, step] : llvm::enumerate(steps)) {
        auto system = map.symbols.lookup<SystemOp>(step.run.getSystem());
        if (!system)
          continue;
        std::vector<int> known(map.states.size());
        for (size_t s = 0; s < known.size(); ++s)
          known[s] = s;
        found[i] = map.flows(system, known);
      }
      if (map.states.size() == before || map.tooMany)
        break;
    }
    if (map.tooMany)
      module.emitWarning() << "the world map has the first " << mostStates
                           << " states of its entities, not all";
    for (auto [i, state] : llvm::enumerate(map.states)) {
      ArchetypeOp archetype = map.archetypes[state.archetype];
      SmallVector<std::string> present;
      for (auto [k, ref] : llvm::enumerate(map.optionals(archetype)))
        if (state.present[k])
          present.push_back(field(cast<FlatSymbolRefAttr>(ref).getValue()));
      os << "state\t" << i << "\t" << state.archetype << "\t"
         << joined(present) << "\n";
    }
    unsigned stepNumber = 0;
    for (auto [i, step] : llvm::enumerate(steps)) {
      if (found[i].empty())
        continue;
      for (const Flow &flow : found[i])
        os << "flow\t" << stepNumber << "\t"
           << field(step.schedule.getSymName()) << "\t" << step.position
           << "\t" << field(step.run.getSystem()) << "\t" << flow.from << "\t"
           << flow.to << "\t" << flow.kind << "\t" << flow.place << "\n";
      ++stepNumber;
    }
  }
};

} // namespace
