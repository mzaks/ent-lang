// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-translate --ent-to-c-header %s | FileCheck %s --check-prefix=HEADER

// A tree that is not sorted has no table of edges: a slot per entity key
// for the entity's one edge, and links from child to child.
ent.component @Node (v: i32)
ent.relation @Under (w: i32) from @Node to @Node tree capacity 16
ent.archetype @Nodes (@Node) capacity 8

// A host sets an entity's edge in its slot, and reads its parent there.
// HEADER: // Relation @Under, a tree
// HEADER: static inline uint64_t ent_Under_slot(ent_entity id) {
// HEADER: static inline ent_entity ent_Under_parent(ent_world *world, ent_entity id) {
// HEADER: static inline bool ent_Under_connect(ent_world *world, ent_entity source,
// HEADER:   uint64_t at = ent_Under_slot(source);
// HEADER:   ent__Under_owner(world)[at] = (ent_entity)(source + 1);
// HEADER:   ent_Under_w(world)[at] = value_w;

// Connecting calls the tree's function, which takes the edge in on the
// spot where it can; the relation is looked over once, when the system
// ends, and built again only if that could not be.
// CHECK-LABEL: func.func private @link(
// CHECK:       call @ent_connect_Under(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}})
// CHECK-NEXT:  call @ent_sort_Under(%{{.*}})
// CHECK-NEXT:  return
ent.system @link(%a: !ent.entity, %b: !ent.entity, %w: i32) {
  ent.connect @Under %a, %b (%w) : i32
}

// The edges into an entity: as many as it has children, from the first
// to the next.
// CHECK-LABEL: func.func private @sum(
// CHECK:       scf.for
// CHECK:         %[[COUNT:.*]] = memref.load %{{.*}}[%[[KEY:.*]]] : memref<8xi32>
// CHECK:         %[[FIRST:.*]] = memref.load %{{.*}}[%[[KEY]]] : memref<8xi32>
// CHECK:         scf.for %{{.*}} = %{{.*}} to %{{.*}} step %{{.*}} iter_args(%[[CHILD:.*]] = %{{.*}}) -> (index) {
// CHECK-NEXT:      %[[SLOT:.*]] = arith.subi %[[CHILD]], %{{.*}}
// CHECK-NEXT:      %[[NEXT:.*]] = memref.load %{{.*}}[%[[SLOT]]] : memref<8xi32>
// CHECK:           scf.yield
// CHECK:       return
ent.system @sum() {
  ent.query (%n: !ent.ref<@Node, mut>) {
    ent.edges @Under in (%s: !ent.ref<@Under>, %child: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Under> -> i32
      ent.set %n "v", %w : !ent.ref<@Node, mut>, i32
    }
  }
}

ent.schedule @step(%a: !ent.entity, %b: !ent.entity, %w: i32) {
  ent.run @link(%a, %b, %w) : !ent.entity, !ent.entity, i32
  ent.run @sum()
}

// The connect: an entity is not its own parent, the slot of its key takes
// the edge, and the list either gets it or the relation is unclean.
// CHECK-LABEL: func.func private @ent_sort_Under(
// CHECK:       cf.assert %{{.*}}, "@Under is a tree, but an entity is its own ancestor"
// CHECK-LABEL: func.func private @ent_connect_Under(%{{.*}}: i32, %{{.*}}: i32, %{{.*}}: i32, %{{.*}}: memref
// CHECK:       cf.assert %{{.*}}, "@Under is a tree, but an entity is its own ancestor"
// CHECK:       cf.assert %{{.*}}, "ent.connect exceeds the capacity of @Under"
