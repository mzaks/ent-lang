#ifndef ENT_WORLD_H
#define ENT_WORLD_H

#include "Ent/EntOps.h"
#include "Ent/Structure.h"

#include <optional>

#include "llvm/ADT/DenseMap.h"

namespace mlir::ent {

/// One field of one component in one archetype's table, the presence of
/// an optional component, or a stamp.
struct WorldColumn {
  StringAttr component;
  /// Empty for the presence byte of an optional component.
  StringAttr field;
  Type type;
  /// Byte offset of the column's first element in the world arena.
  uint64_t offset;
  /// For a stamp column (i64 ticks): which stamp; `component` is the
  /// stamp's and `field` is empty.
  std::optional<Stamp> stamp = std::nullopt;

  bool isStamp() const { return stamp.has_value(); }
  bool isPresence() const { return !stamp && field.getValue().empty(); }
};

/// A move of entities out of an archetype, into `target`, caused by adding
/// or removing `component`.
struct WorldMove {
  ArchetypeOp target;
  StringAttr component;
  bool add;
  /// The action code pending rows use for this move (despawn is 0).
  unsigned code;
  /// For an add: one column per field of the component, holding the values
  /// to initialise it with, by pending slot.
  SmallVector<WorldColumn> values;
};

struct WorldArchetype {
  ArchetypeOp op;
  int64_t capacity;
  /// Position of the archetype's entity count in the counts array.
  unsigned index;
  SmallVector<WorldColumn> columns;
  /// The id of the entity in each row (i64).
  uint64_t idOffset = 0;
  /// For an archetype that some query despawns from or moves entities out
  /// of: the rows to remove at the end of such a query (one i32 per slot of
  /// capacity) and where their number is kept. Zero if there is none.
  uint64_t pendingOffset = 0;
  uint64_t pendingCountOffset = 0;
  /// With moves: what to do with each pending row (0 despawn, otherwise a
  /// move's code), one i32 per slot.
  uint64_t pendingActionOffset = 0;
  SmallVector<WorldMove> moves;

  bool hasPending() const { return pendingOffset != 0; }

  /// For an archetype holding entities of a tree declared `sorted` (its
  /// rows are kept with the entities without a parent first, then the
  /// others by their depth in the tree, so every parent's depth is before
  /// its children's): the relation; in the header (i64) how many entities
  /// have no parent; and for sorting, the row each row goes to (i32) and a
  /// copy of every column (in the order of `columns`) and of the ids.
  ///
  /// `levelStart` has, per depth d from 1 (i32), the first row of that
  /// depth; the entry after the last depth is the number of rows.
  ///
  /// Where the tree lives in this archetype alone, the others are in the
  /// order the tree lists them, every parent before its children and the
  /// children of one parent next to each other, in their parents' order.
  /// Each row has the row of its parent (i32, -1 for none) and the rows
  /// its children are in, from `childBegin` to `childEnd` (i32; equal for
  /// none).
  ///
  /// Where it lives in several, each row has its parent's packed location
  /// (archetype and row, at the width of a location).
  StringAttr sortedBy;
  uint64_t rootCountOffset = 0;
  uint64_t parentRowOffset = 0;
  uint64_t parentLocationOffset = 0;
  uint64_t childBeginOffset = 0;
  uint64_t childEndOffset = 0;
  uint64_t levelStartOffset = 0;
  int64_t levelCapacity = 0;
  uint64_t newRowOffset = 0;
  SmallVector<uint64_t> columnScratchOffsets;
  uint64_t idScratchOffset = 0;

  bool isSorted() const { return static_cast<bool>(sortedBy); }

  /// The move for adding or removing `component`, or null.
  const WorldMove *findMove(StringAttr component, bool add) const;

  /// The column of `component`.`field` (or its presence, for an empty
  /// field), or null.
  const WorldColumn *find(StringAttr component, StringAttr field) const;
  /// The column holding `stamp`, or null if the archetype does not store
  /// it.
  const WorldColumn *findStamp(const Stamp &stamp) const;
};

/// One field of a resource: stored once.
struct WorldResourceField {
  StringAttr field;
  Type type;
  /// Byte offset of the field in the world arena.
  uint64_t offset;
};

struct WorldResource {
  ResourceOp op;
  SmallVector<WorldResourceField> fields;

  /// The field named `field`, or null.
  const WorldResourceField *find(StringAttr field) const;
};

/// The values one `ent.apply` sends from the entities of one archetype: a
/// target id and a value per row. A row whose entity sent nothing holds the
/// all-ones id (`ENT_NO_ENTITY`). An `ent.accumulate` uses the same buffers;
/// its ids only say whether the row sent a value (0) or not.
struct WorldApplyBuffer {
  /// Index of the source archetype.
  unsigned archetype;
  uint64_t idOffset;
  uint64_t valueOffset;
};

/// The buffers of one `ent.apply`, one per archetype its query matches.
/// An apply inside `ent.edges` sends a value per edge: its per-row ids
/// only say whether the row ran the edge loop (0) or not, and the target
/// ids and values are kept per edge, at the edge's position in the loop's
/// order (the sorted table for `out`, the index by target for `in`).
struct WorldApply {
  Type type;
  SmallVector<WorldApplyBuffer> buffers;
  /// For an apply inside `ent.edges`: per edge, a target id and a value.
  uint64_t edgeIdOffset = 0;
  uint64_t edgeValueOffset = 0;
  int64_t edgeCapacity = 0;

  /// The buffer for source archetype `archetype` (its index).
  const WorldApplyBuffer &find(unsigned archetype) const;
};

/// The edges of one relation: a table of at most `capacity` edges (source
/// id, target id, a column per field), the first `count` of which are
/// sorted by one end, so the edges of the entity with key k at that end
/// are those from `sorted[k]` to `sorted[k + 1]` (compressed sparse rows).
/// Keys are entity slots, or for row ids the id itself (see
/// WorldLayout::entityKeys). The table is sorted by source, or by target
/// where the program visits only incoming edges, so that every edge loop
/// reads its edges' columns in order. Where the program visits edges both
/// ways, `index` holds the offsets by target and `indexEdges` the positions
/// of those edges in the table. Either way an entity's edges are in the
/// same order: by source, then in the order they were connected.
///
/// `ent.connect` appends after `count` and marks the relation unclean
/// (`clean` 0, which is also how a new world starts); disconnects mark
/// edges dead. Sorting again (a stable counting sort, through the scratch
/// columns) drops dead edges and those whose source or target is no
/// longer alive, and makes the relation clean.
///
/// A tree is sorted by source and has the index by target: an entity's
/// parent is the target of its one edge, its children are the sources of
/// the edges to it. Sorting keeps the last edge connected from each source
/// and lists the entities that have a parent in `order`, parents before
/// their children (breadth first from the entities without a parent); an
/// entity on a cycle would never be listed, which stops the program.
///
/// A tree that is not `sorted` is linked instead: it has no table of
/// edges. Every entity key has a slot for the one edge from its entity:
/// the source's id (all ones for none; an edge is its entity's if it
/// holds the entity's id), the target's, and the fields. `sourceOffset`,
/// `targetOffset`, `fields` and `deadOffset` are those columns, of
/// `slots` elements. The edges to an entity are a list through the slots
/// of their sources, in the order they were connected (links are a key
/// and one, 0 for none): first and last child and their number per key,
/// next and previous sibling per slot. The entities with a parent are in
/// `order` as for any tree, parents before their children, with their
/// parents' ids next to them; `position` has where each key's is (and
/// one). Connecting keeps all of it as it goes: a new leaf goes to the
/// list's end, an entity put under one that is before it has its parent's
/// id changed in place, and one put under a later entity goes to the end
/// with everything below it, leaving entries of all ones behind. Where
/// the list has no room for that, and for a disconnect or a host's
/// connect, the relation is marked unclean; unclean, it is built
/// again from the slots: dead edges dropped, children by key, and the
/// list by key with every entity's ancestors put before it.
struct WorldRelation {
  RelationOp op;
  int64_t capacity;
  /// Elements of the edge columns: `capacity`, or for a linked tree the
  /// number of entity keys.
  int64_t slots = 0;
  bool linked = false;
  uint64_t firstChildOffset = 0;
  uint64_t lastChildOffset = 0;
  uint64_t childCountOffset = 0;
  uint64_t nextSiblingOffset = 0;
  uint64_t previousSiblingOffset = 0;
  uint64_t positionOffset = 0;

  /// In the header (i64): the number of edges, and whether the sorted
  /// table and offsets are current.
  uint64_t countOffset = 0;
  uint64_t cleanOffset = 0;
  uint64_t sourceOffset = 0;
  uint64_t targetOffset = 0;
  /// One column per field; `component` is the relation's name.
  SmallVector<WorldColumn> fields;
  /// One i8 per edge, 1 once disconnected; 0 if nothing disconnects.
  uint64_t deadOffset = 0;
  /// Width of an offset or edge position: 32 or 64 bits.
  unsigned offsetBits = 32;
  /// Whether the table is sorted by target rather than by source.
  bool byTarget = false;
  /// Offsets by the end the table is sorted by, entityKeys + 1 of them.
  uint64_t sortedOffset = 0;
  /// By target, if the program visits edges both ways: offsets and the
  /// table positions of the edges.
  uint64_t indexOffset = 0;
  uint64_t indexEdgesOffset = 0;
  /// For sorting: a cursor per key and a copy of every column.
  uint64_t cursorOffset = 0;
  uint64_t sourceScratchOffset = 0;
  uint64_t targetScratchOffset = 0;
  SmallVector<uint64_t> fieldScratchOffsets;
  /// For a tree: the ids of the entities with a parent, parents before
  /// children (one per edge), next to each its parent's id, and in the
  /// header (i64) how many.
  bool tree = false;
  uint64_t orderOffset = 0;
  uint64_t orderParentOffset = 0;
  /// For a sorted tree, in the header (i64): how many edges the table had
  /// when it was last sorted. Those are in order, with the offsets by
  /// source for them; a connect of an entity that has one of them sets it
  /// in place.
  uint64_t sortedCountOffset = 0;
  /// For a linked tree where ids are not rows: next to each entity in the
  /// list its packed location and its parent's, so a walk of the list
  /// goes to their rows without the entity table; and in the header (i64)
  /// whether those are stale, which they are once a row of an archetype
  /// that can hold the tree's entities has moved. Stale, they are read
  /// from the entity table again where the relation is next looked over.
  uint64_t orderLocationOffset = 0;
  uint64_t orderParentLocationOffset = 0;
  uint64_t staleOffset = 0;
  bool hasLocations() const { return orderLocationOffset != 0; }
  /// For a linked tree that a reactive query follows events down (see
  /// cascadeFollowsEvents): a bit per element of the list, in i64 words,
  /// set for the entities such a query has yet to look at. All are 0
  /// between queries.
  uint64_t marksOffset = 0;
  int64_t markWords() const { return (orderCapacity + 63) / 64; }
  /// Elements of the list: an edge each, or for a linked tree twice that,
  /// which leaves room for the entries of entities that moved to its end
  /// (their old ones are all ones, and skipped) before it is made again.
  int64_t orderCapacity = 0;
  /// The indices of the archetypes a `sorted` tree keeps in its order, and
  /// in the header (i64) the depth of the tree: of its deepest entity,
  /// where an entity without a parent has 0.
  SmallVector<unsigned, 2> sortedArchetypes;
  uint64_t depthOffset = 0;
  /// The one archetype the tree lives in, or -1.
  int sortedArchetype() const {
    return sortedArchetypes.size() == 1 ? int(sortedArchetypes.front()) : -1;
  }
  /// The component the sources (0) and the targets (1) are trusted to
  /// have for as long as an edge exists (see getTrustedEndpoint), decided
  /// for the program as it was written: lowering takes the ops that
  /// decide it away, system by system.
  FlatSymbolRefAttr trusted[2];
  FlatSymbolRefAttr getTrusted(bool target) const { return trusted[target]; }
  uint64_t orderCountOffset = 0;

  bool hasIndex() const { return indexOffset != 0; }
  /// Whether a loop visiting incoming (`in`) or outgoing edges reads the
  /// table in order, rather than through the index.
  bool isSorted(bool in) const { return in == byTarget; }
  /// The column of `field`, or null.
  const WorldColumn *find(StringAttr field) const;
};

/// The edges one `ent.connect` inside a query adds, from the entities of
/// each archetype the query matches: per row a source id (all ones if the
/// row added none), a target id and the field values.
struct WorldConnect {
  struct Buffer {
    unsigned archetype;
    uint64_t sourceOffset;
    uint64_t targetOffset;
    SmallVector<uint64_t> valueOffsets;
  };
  SmallVector<Buffer> buffers;

  const Buffer &find(unsigned archetype) const;
};

/// The event log of one observed stamp: (entity id, tick) entries, one per
/// entity whose stamp an event set to a new tick, so that a reactive query
/// can visit the entities with events instead of scanning every row.
///
/// The log is split into segments, each a ring of its own with its own
/// count of entries ever appended (on a cache line of its own). An event at
/// row r of an archetype holding n entities goes to segment r * segments /
/// n: the threads of a parallel loop, each running a contiguous range of
/// rows, mostly append to segments of their own, and an atomic add keeps
/// the few shared at range boundaries correct. A query that finds more
/// entries in some segment than it holds since it last read it (it was
/// overwritten) scans instead.
struct WorldLog {
  /// Entries in all (a power of two); 0 if the stamp has no log.
  int64_t capacity = 0;
  /// Segments (a power of two) and entries per segment.
  int64_t segments = 0;
  int64_t segmentCapacity = 0;
  /// The rings, segment after segment: ids at the id width, ticks as i64.
  uint64_t idsOffset = 0;
  uint64_t ticksOffset = 0;
  /// In the header, per segment on a cache line: the number of entries ever
  /// appended (i64), right after it the smallest position of the log's
  /// readers, and then the count as the last event left it that found the
  /// segment full for that reader and was not appended. A reader whose
  /// position is before that has lost an event.
  uint64_t countsOffset = 0;
  /// In the header: where each segment ended when its current reader
  /// started (one i64 per segment). Readers never run concurrently.
  uint64_t endsOffset = 0;
  /// Per reactive query reading the log: where its positions (one i64 per
  /// segment) are in the header.
  SmallVector<uint64_t> readerOffsets;
  static constexpr int64_t kMaxSegments = 64;
  static constexpr int64_t kMinSegmentCapacity = 64;
  static constexpr int64_t kSegmentStride = 64;

  bool exists() const { return capacity != 0; }
  /// Header offsets of a segment's count and slowest reader's position.
  uint64_t countOffset(int64_t segment) const {
    return countsOffset + segment * kSegmentStride;
  }
  uint64_t slowestOffset(int64_t segment) const {
    return countOffset(segment) + 8;
  }
};

/// How entity ids and their bookkeeping are laid out, chosen from the
/// capacities and from which structural changes the program makes:
///
/// - Rows: nothing despawns or moves and no archetype is sorted by a tree,
///   so an entity keeps its row forever and its id is `archetype << rowBits
///   | row`. No entity table, no id column.
/// - Slots: entities move but never die, so slots are never reused; an id
///   is a slot of the entity table, which holds only locations.
/// - Generational: entities die and slots are reused; an id is
///   `generation << slotBits | slot`, and the table holds a generation per
///   slot besides the location. Freed slots are chained through their
///   locations.
///
/// A location packs `archetype << rowBits | row`. Ids are 32 bits wide when
/// that leaves enough generation bits (the module attribute
/// `ent.min_generation_bits`, 8 by default), 64 otherwise or when the
/// module attribute `ent.entity_id_bits = 64` asks for it.
struct EntityScheme {
  enum Kind { Rows, Slots, Generational };
  Kind kind = Generational;
  /// Width of an id: its storage in id columns, relation fields and the C
  /// API.
  unsigned idBits = 64;
  unsigned slotBits = 32;
  /// Bits of the generation that are used; it wraps around at this width.
  unsigned generationBits = 32;
  /// Width of a stored generation (8, 16 or 32).
  unsigned generationStorageBits = 32;
  /// Bits of the row in a packed location or a Rows id; the archetype index
  /// is above.
  unsigned rowBits = 32;
  /// Width of a stored location (32 or 64).
  unsigned locationBits = 64;

  bool hasIds() const { return kind != Rows; }
  bool hasGenerations() const { return kind == Generational; }
};

/// Static layout of the whole world in one arena, computed from the
/// archetypes' components and capacities and from the resources.
///
/// The arena starts with one i64 entity count per archetype, in declaration
/// order. Resources follow, each starting on a cache line, with naturally
/// aligned fields. Columns follow, one per field, ordered by archetype,
/// then by the archetype's components, then by the component's fields; an
/// optional component's fields are followed by its presence column (one
/// i8 per entity, 1 if present). Each column starts at a 64-byte boundary,
/// pushed 17 cache lines beyond the end of the previous column: columns
/// laid out back to back from a page-aligned base would otherwise tend to
/// start at the same cache set, which costs a single core 7-8% on the
/// example (see bench/RESULTS.md). Each archetype's columns are followed by
/// its id column; an archetype that entities are despawned from or moved
/// out of also gets a pending counter in the header and pending lists after
/// its id column. An archetype's stamp columns (see StampPlan) come after
/// its component columns. The buffers of `ent.apply` follow the
/// archetypes, and the entity table comes last. Programs with reactive
/// queries keep a tick counter and each reactive query's last tick in the
/// header.
struct WorldLayout {
  static constexpr uint64_t kArenaAlignment = 16384;
  static constexpr uint64_t kColumnAlignment = 64;
  static constexpr uint64_t kStagger = 17 * 64;

  SmallVector<WorldArchetype, 1> archetypes;
  SmallVector<WorldResource> resources;
  /// One entry per `ent.apply` and `ent.accumulate` in the module, in walk
  /// order; the lowering tags each op with its index (see kApplyIndexAttr).
  SmallVector<WorldApply> applies;
  static constexpr llvm::StringLiteral kApplyIndexAttr = "ent.apply_index";
  /// Relations in declaration order, and one entry per `ent.connect`
  /// inside a query, in walk order (tagged with kConnectIndexAttr).
  SmallVector<WorldRelation, 1> relations;
  SmallVector<WorldConnect> connects;
  static constexpr llvm::StringLiteral kConnectIndexAttr =
      "ent.connect_index";
  /// The number of entity keys relations index their edges by: the
  /// entity table's size, or for row ids `archetypes << rowBits`.
  int64_t entityKeys = 0;

  /// The layout of `relation`; it must be declared in the module.
  const WorldRelation &getRelation(StringAttr relation) const;
  /// Whether the reactive, cascading `query` goes only where its events
  /// lead, instead of through its whole tree: from the entities in its
  /// triggers' event logs, and from each one it changes, down to the
  /// children. That needs the tree's links (not a sorted tree), parents
  /// first, a log for every trigger, a ref up the tree (so that the
  /// entities without a parent, which are not in the tree's list, are not
  /// visited), and for a trigger up the tree the parent itself to be the
  /// ancestor it means (the relation's targets all have the component).
  bool cascadeFollowsEvents(QueryOp query) const;

  /// Reactive queries: the stamps and where they are stored, the tick
  /// counter (an i64, 0 if there are no reactive queries), and per reactive
  /// query, in walk order, the tick at which it last started (an i64). The
  /// lowering tags each reactive query with its index (kReactiveIndexAttr).
  StampPlan stamps;
  uint64_t tickOffset = 0;
  SmallVector<uint64_t> reactiveOffsets;
  /// One log per stamp, in the order of `stamps.getStamps()`.
  SmallVector<WorldLog> logs;
  /// Per reactive query (by index) and trigger (in order): the header
  /// offset of the query's positions in the trigger's log (one i64 per
  /// segment), 0 if none.
  SmallVector<SmallVector<uint64_t>> readPositions;

  /// The log of `stamp`, or null if it has none.
  const WorldLog *findLog(const Stamp &stamp) const;
  static constexpr llvm::StringLiteral kReactiveIndexAttr =
      "ent.reactive_index";

  /// How entity ids are represented; see EntityScheme.
  EntityScheme entities;
  /// The entity table, indexed by an id's slot: the slot's generation and
  /// the packed location (archetype and row) of the entity living in it. A
  /// free slot's location holds the next free slot plus one (0 ends the
  /// list), so the free list needs no storage of its own. One entry per
  /// slot of total capacity. The next never-used slot and the head of the
  /// free list (plus one) are i64 counters in the header.
  int64_t entityCapacity = 0;
  uint64_t nextSlotOffset = 0;
  uint64_t freeHeadOffset = 0;
  uint64_t generationOffset = 0;
  uint64_t locationOffset = 0;
  /// Bytes taken by the counts at the start of the arena.
  uint64_t countsBytes = 0;
  /// Bytes taken by the counts and resources, which a new world zeroes.
  uint64_t headerBytes = 0;
  /// What else a new world zeroes, as (offset, bytes): the owners of a
  /// linked tree's slots, where zero says that a slot holds no edge. The
  /// memory a world is given need not be zero, and what it holds could be
  /// taken for the edges of entities that exist.
  SmallVector<std::pair<uint64_t, uint64_t>, 0> zeroed;
  /// Total size of the arena.
  uint64_t totalBytes = 0;

  /// Computes the layout, or emits an error on `module` and fails if a
  /// field has a type the world cannot store.
  static FailureOr<WorldLayout> compute(ModuleOp module);

  /// The layout of `resource`; it must be declared in the module.
  const WorldResource &getResource(StringAttr resource) const;
};

/// Bytes one element of `type` takes in world storage, or 0 if the type is
/// not supported (i1, i8, i16, i32, i64, index, f32, f64 and entity ids
/// are).
uint64_t getStorageBytes(Type type);

} // namespace mlir::ent

#endif // ENT_WORLD_H
