// RUN: ecs-opt %s --ecs-lower-to-loops --canonicalize | FileCheck %s

// 32768 units give the hp log 4096 entries, in 64 segments of 64. A write
// at row r of n units appends to segment r * 64 / n, so the contiguous row
// ranges of a parallel loop's threads mostly use segments of their own;
// segment s's count is the i64 at s * 8 of the counts.
ecs.component @H (hp: f32)
ecs.component @B (w: f32)
ecs.archetype @A (@H, @B) capacity 32768

// CHECK-LABEL: func.func private @hurt(
// CHECK-DAG:  %[[COUNTS:.*]] = memref.view %{{.*}} : memref<{{.*}}xi8> to memref<512xi64>
// CHECK:      %[[N:.*]] = arith.index_cast %[[ROWS:.*]] : i64 to index
// CHECK:      scf.for %[[ROW:.*]] = %c0 to %[[N]] step
// CHECK:        %[[R:.*]] = arith.index_cast %[[ROW]] : index to i64
// CHECK-NEXT:   %[[SCALED:.*]] = arith.muli %[[R]], %c64_i64
// CHECK-NEXT:   %[[SEGMENT:.*]] = arith.divui %[[SCALED]], %[[ROWS]]
// CHECK:        %[[BASE:.*]] = arith.muli %[[SEGMENT]], %c8_i64
// CHECK-NEXT:   %[[AT:.*]] = arith.index_cast %[[BASE]] : i64 to index
// CHECK:        memref.load %[[COUNTS]][%[[AT]]]
ecs.system @hurt(%d: f32) writes [@H] {
  ecs.query (%h: !ecs.ref<@H, mut>) {
    ecs.set %h "hp", %d : !ecs.ref<@H, mut>, f32
  }
}

// The reader notes where every segment ends and whether any overflowed,
// and walks the segments one after another.
// CHECK-LABEL: func.func private @watch(
// CHECK:      %[[SCAN:.*]] = scf.for %[[SEG:.*]] = %c0 to %c64 step %c1 iter_args(%[[ANY:.*]] = %{{.*}}) -> (i1) {
// CHECK:        %[[LOST:.*]] = arith.cmpi sgt, %{{.*}}, %c64_i64 : i64
// CHECK-NEXT:   %[[OR:.*]] = arith.ori %[[ANY]], %[[LOST]] : i1
// CHECK-NEXT:   scf.yield %[[OR]] : i1
// CHECK:      scf.if %[[SCAN]] {
// CHECK:      } else {
// CHECK-NEXT:   scf.for %[[S:.*]] = %c0 to %c64 step %c1 {
ecs.system @watch() reads [@H] writes [@B] {
  ecs.query (%h: !ecs.ref<@H>, %b: !ecs.ref<@B, mut>) on [changed @H "hp"] {
    %x = ecs.get %h "hp" : !ecs.ref<@H> -> f32
    ecs.set %b "w", %x : !ecs.ref<@B, mut>, f32
  }
}
