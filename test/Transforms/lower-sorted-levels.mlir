// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s

// A sorted tree whose entities are in two archetypes: each has its rows
// by depth and says where each depth starts, and a row has its parent's
// location, archetype and row.
ent.component @Node (rain: f32, flow: f32)
ent.component @Still (level: f32)
ent.relation @Flows () from @Node to @Node tree sorted capacity 16
ent.archetype @Cell (@Node) capacity 8
ent.archetype @Pool (@Node, @Still) capacity 8

// Children first: the depths from the last up, in each the rows of one
// archetype and then the other's, from their last down. The parent is in
// one of the two, which a branch on its location's archetype tells; the
// last needs no test.
// CHECK-LABEL: func.func private @run(
// CHECK:       %[[DEPTHS:.*]] = arith.index_cast %{{.*}} : i64 to index
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to %[[DEPTHS]] step %[[ONE:.*]] {
// CHECK-NEXT:    %[[D:.*]] = arith.subi %[[DEPTHS]], %[[I]]
// CHECK-NEXT:    %[[NEXT:.*]] = arith.addi %[[D]], %[[ONE]]
// CHECK-NEXT:    %[[FROM32:.*]] = memref.load %[[STARTS:.*]][%[[D]]] : memref<18xi32>
// CHECK:         %[[TO32:.*]] = memref.load %[[STARTS]][%[[NEXT]]] : memref<18xi32>
// CHECK:         scf.for %[[J:.*]] = %{{.*}} to %{{.*}} step %[[ONE]] {
// CHECK:           %[[LOCATION:.*]] = memref.load %{{.*}}[%[[ROW:.*]]] : memref<8xi32>
// CHECK:           %[[WHERE:.*]] = arith.shrui %[[LOCATION]], %{{.*}}
// CHECK:           %[[MASKED:.*]] = arith.andi %[[LOCATION]], %{{.*}}
// CHECK-NEXT:      %[[PARENT:.*]] = arith.index_cast %[[MASKED]] : i32 to index
// CHECK:           %[[HERE:.*]] = arith.cmpi eq, %[[WHERE]], %{{.*}}
// CHECK-NEXT:      scf.if %[[HERE]] {
// CHECK:             memref.store %{{.*}}, %{{.*}}[%[[PARENT]]] : memref<8xf32>
// CHECK-NEXT:      } else {
// CHECK:             memref.store %{{.*}}, %{{.*}}[%[[PARENT]]] : memref<8xf32>
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK:         scf.for
// CHECK-NOT:   scf.while
// CHECK:       return
ent.system @run() {
  ent.query (%n: !ent.ref<@Node>, %d: !ent.ref<@Node, mut, up @Flows>)
      cascade @Flows leaves first {
    %own = ent.get %n "flow" : !ent.ref<@Node> -> f32
    ent.combine %d "flow" add %own : !ent.ref<@Node, mut, up @Flows>, f32
  }
}

// A component not every archetype of the tree always has: the ancestor
// that has it is searched for, from the entity.
// CHECK-LABEL: func.func private @level(
// CHECK:       scf.while
// CHECK:       return
ent.system @level() {
  ent.query (%n: !ent.ref<@Node, mut>, %s: !ent.ref<@Still, up @Flows>)
      cascade @Flows {
    %level = ent.get %s "level" : !ent.ref<@Still, up @Flows> -> f32
    ent.set %n "flow", %level : !ent.ref<@Node, mut>, f32
  }
}

ent.schedule @step() {
  ent.run @run()
  ent.run @level()
}
