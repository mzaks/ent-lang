// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-print-access 2>&1 | FileCheck %s --check-prefix=ACCESS
// RUN: ent-translate --ent-to-c-header %s | FileCheck %s --check-prefix=HEADER

// A tree whose entities are stored in its order: the rows of their
// archetype are the entities without a parent, then the others, every one
// after its parent.
ent.component @Node (rain: f32, flow: f32)
ent.relation @Flows () from @Node to @Node tree sorted capacity 16
ent.archetype @Cell (@Node) capacity 16

// Ids are never rows where rows move, and a host that spawns leaves the
// archetype unsorted until a schedule starts.
// HEADER: typedef uint32_t ent_entity;
// HEADER: static inline ent_entity ent_Cell_spawn(ent_world *world) {
// HEADER:   *(int64_t *)((char *)world + {{[0-9]+}}) = 0; // unsorted
// HEADER: static inline ent_entity *ent_Cell_id(ent_world *world) {

// Children first is the rows after the roots from the last down, each
// with its parent's row at hand: no list of ids, no id looked up.
// CHECK-LABEL: func.func private @run(
// (The archetype is counted when the query starts.)
// CHECK:       arith.index_cast %{{.*}} : i64 to index
// CHECK:       %[[ROWS:.*]] = arith.index_cast %{{.*}} : i64 to index
// CHECK:       %[[ROOTS:.*]] = arith.index_cast %{{.*}} : i64 to index
// CHECK:       %[[N:.*]] = arith.subi %[[ROWS]], %[[ROOTS]]
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to %[[N]] step %[[ONE:.*]] {
// CHECK-NEXT:    %[[LAST:.*]] = arith.subi %[[ROWS]], %[[ONE]]
// CHECK-NEXT:    %[[ROW:.*]] = arith.subi %[[LAST]], %[[I]]
// CHECK-NEXT:    %[[P:.*]] = memref.load %{{.*}}[%[[ROW]]] : memref<16xi32>
// CHECK-NEXT:    %[[PARENT:.*]] = arith.index_cast %[[P]] : i32 to index
// CHECK-NEXT:    %[[OWN:.*]] = memref.load %[[FLOW:.*]][%[[ROW]]] : memref<16xf32>
// CHECK-NEXT:    %[[OLD:.*]] = memref.load %[[FLOW]][%[[PARENT]]] : memref<16xf32>
// CHECK-NEXT:    %[[SUM:.*]] = arith.addf %[[OLD]], %[[OWN]]
// CHECK-NEXT:    memref.store %[[SUM]], %[[FLOW]][%[[PARENT]]] : memref<16xf32>
// CHECK-NEXT:  }
// CHECK-NEXT:  return
ent.system @run() {
  ent.query (%n: !ent.ref<@Node>, %d: !ent.ref<@Node, mut, up @Flows>)
      cascade @Flows leaves first {
    %own = ent.get %n "flow" : !ent.ref<@Node> -> f32
    ent.combine %d "flow" add %own : !ent.ref<@Node, mut, up @Flows>, f32
  }
}

// Parents first: the roots, then the rows after them going up.
// CHECK-LABEL: func.func private @down(
// CHECK:       scf.for %{{.*}} = %{{.*}} to %[[ROOTS:.*]] step
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to %{{.*}} step %{{.*}} {
// CHECK-NEXT:    %[[ROW:.*]] = arith.addi %{{.*}}, %[[I]]
// CHECK:       return
ent.system @down() {
  ent.query (%n: !ent.ref<@Node, mut>) cascade @Flows {
    %own = ent.get %n "rain" : !ent.ref<@Node, mut> -> f32
    ent.set %n "flow", %own : !ent.ref<@Node, mut>, f32
  }
}

// What changes the tree, or which entities the archetype holds, moves
// its rows: it writes every column, and the tree's edges with them.
// ACCESS: remark: reads entities; writes Flows.edges, entities, Cell.count, Cell.id, Cell.Node.rain, Cell.Node.flow
// ACCESS-NEXT: ent.system @link(
// Connected outside a query: sorted once, when the system ends.
// CHECK-LABEL: func.func private @link(
// CHECK-NOT:   call @ent_sort_Flows
// CHECK:       call @ent_sort_Flows(%{{.*}})
// CHECK-NEXT:  return
ent.system @link(%a: !ent.entity, %b: !ent.entity) {
  ent.connect @Flows %a, %b ()
  ent.connect @Flows %b, %a ()
}
// ACCESS: remark: reads nothing; writes entities, Cell.count, Cell.id, Cell.Node.rain, Cell.Node.flow, Flows.edges
// ACCESS-NEXT: ent.system @grow(
// The new entity is out of order: marked, and sorted when the system
// ends rather than at once.
// CHECK-LABEL: func.func private @grow(
// CHECK:       memref.store %{{.*}}, %{{.*}}[%{{.*}}] : memref<1xi64>
// CHECK:       call @ent_sort_Flows(%{{.*}})
// CHECK-NEXT:  return
ent.system @grow() {
  %zero = arith.constant 0.0 : f32
  %id = ent.spawn @Cell(%zero, %zero) : f32, f32
}

ent.schedule @step() {
  ent.run @run()
  ent.run @down()
  ent.run @grow()
}
