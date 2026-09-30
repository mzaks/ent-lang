// RUN: ecs-opt %s --ecs-lower-to-loops --canonicalize | FileCheck %s

ecs.component @H (hp: f32)
ecs.component @B (w: f32)
ecs.archetype @A (@H, @B) capacity 8

// A write to an observed field stores the current tick (one past the
// counter, loaded once before the loop) in the field's stamp column.
// CHECK-LABEL: func.func private @hurt(
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[HP:.*]] = memref.view %{{.*}}[%c1152][] : memref<16384xi8> to memref<8xf32>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3456][] : memref<16384xi8> to memref<8xi64>
// CHECK:      %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[TICK:.*]] = arith.addi %[[C]], %c1_i64
// CHECK:      scf.for %[[ROW:.*]] =
// CHECK-NEXT:   memref.store %{{.*}}, %[[HP]][%[[ROW]]]
// CHECK-NEXT:   memref.store %[[TICK]], %[[STAMP]][%[[ROW]]]
ecs.system @hurt(%d: f32) writes [@H] {
  ecs.query (%h: !ecs.ref<@H, mut>) {
    ecs.set %h "hp", %d : !ecs.ref<@H, mut>, f32
  }
}

// A reactive query takes the tick it last started at, and makes the next
// one both its new last tick and the counter. It runs for every row, and
// keeps its stores only where the stamp is newer than what it had seen
// (branch-free, like a body masked by an optional component).
// CHECK-LABEL: func.func private @redraw(
// CHECK-DAG:  %[[LAST:.*]] = memref.view %{{.*}}[%c32][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3456][] : memref<16384xi8> to memref<8xi64>
// CHECK-DAG:  %[[W:.*]] = memref.view %{{.*}}[%c2304][] : memref<16384xi8> to memref<8xf32>
// CHECK:      %[[SEEN:.*]] = memref.load %[[LAST]][%c0]
// CHECK-NEXT: %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[NEXT:.*]] = arith.addi %[[C]], %c1_i64
// CHECK-NEXT: memref.store %[[NEXT]], %[[LAST]][%c0]
// CHECK-NEXT: memref.store %[[NEXT]], %[[COUNTER]][%c0]
// CHECK:      scf.for %[[ROW:.*]] =
// CHECK-NEXT:   %[[S:.*]] = memref.load %[[STAMP]][%[[ROW]]]
// CHECK-NEXT:   %[[NEW:.*]] = arith.cmpi sgt, %[[S]], %[[SEEN]] : i64
// CHECK:        %[[V:.*]] = arith.select %[[NEW]], %{{.*}}, %{{.*}} : f32
// CHECK-NEXT:   memref.store %[[V]], %[[W]][%[[ROW]]]
ecs.system @redraw() reads [@H] writes [@B] {
  ecs.query (%h: !ecs.ref<@H>, %b: !ecs.ref<@B, mut>) on [changed @H "hp"] {
    %x = ecs.get %h "hp" : !ecs.ref<@H> -> f32
    ecs.set %b "w", %x : !ecs.ref<@B, mut>, f32
  }
}

// A spawn stamps its row: the entity has added and changed its components.
// CHECK-LABEL: func.func private @make(
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3456][] : memref<16384xi8> to memref<8xi64>
// CHECK:      %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[TICK:.*]] = arith.addi %[[C]], %c1_i64
// CHECK-NEXT: memref.store %[[TICK]], %[[STAMP]][%{{.*}}] : memref<8xi64>
ecs.system @make(%x: f32) writes [@A] {
  ecs.spawn @A(%x, %x) : f32, f32
}
