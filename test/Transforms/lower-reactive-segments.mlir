// RUN: ent-opt %s --ent-lower-to-loops --canonicalize | FileCheck %s
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1 parallel-min-events=100" \
// RUN:     --canonicalize | FileCheck %s --check-prefix=PAR

// 32768 units give the hp log 4096 entries, in 64 segments of 64. A write
// at row r of n units appends to segment r * 64 / n, so the contiguous row
// ranges of a parallel loop's threads mostly use segments of their own;
// segment s's count is the i64 at s * 8 of the counts.
ent.component @H (hp: f32)
ent.component @B (w: f32)
ent.archetype @A (@H, @B) capacity 32768

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
ent.system @hurt(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H, mut>) {
    ent.set %h "hp", %d : !ent.ref<@H, mut>, f32
  }
}

// The reader notes where every segment ends, whether any overflowed, and
// how many entries are pending, and walks the segments one after another.
// CHECK-LABEL: func.func private @watch(
// CHECK:      %[[STATE:.*]]:2 = scf.for %[[SEG:.*]] = %c0 to %c64 step %c1 iter_args(%[[ANY:.*]] = %{{.*}}, %[[SUM:.*]] = %{{.*}}) -> (i1, i64) {
// CHECK:        %[[OVER:.*]] = arith.cmpi sgt, %{{.*}}, %c64_i64 : i64
// CHECK:        %[[BEHIND:.*]] = arith.cmpi slt, %{{.*}}, %{{.*}} : i64
// CHECK-NEXT:   %[[LOST:.*]] = arith.ori %[[OVER]], %[[BEHIND]] : i1
// CHECK:        %[[OR:.*]] = arith.ori %[[ANY]], %[[LOST]] : i1
// CHECK-NEXT:   scf.yield %[[OR]], %{{.*}} : i1, i64
// CHECK:      scf.if %[[STATE]]#0 {
// CHECK:      } else {
// CHECK-NEXT:   scf.for %[[S:.*]] = %c0 to %c64 step %c1 {

// With parallel entity loops, a walk of at least parallel-min-events
// pending entries (100 here) runs the segments in parallel.
// PAR-LABEL: func.func private @watch(
// PAR:       %[[STATE:.*]]:2 = scf.for
// PAR:       scf.if %[[STATE]]#0 {
// PAR:       } else {
// PAR-NEXT:    %[[MANY:.*]] = arith.cmpi sge, %[[STATE]]#1, %c100_i64 : i64
// PAR-NEXT:    scf.if %[[MANY]] {
// PAR-NEXT:      scf.parallel (%{{.*}}) = (%c0) to (%c64) step (%c1) {
// PAR:         } else {
// PAR-NEXT:      scf.for %{{.*}} = %c0 to %c64 step %c1 {
ent.system @watch() reads [@H] writes [@B] {
  ent.query (%h: !ent.ref<@H>, %b: !ent.ref<@B, mut>) on [changed @H "hp"] {
    %x = ent.get %h "hp" : !ent.ref<@H> -> f32
    ent.set %b "w", %x : !ent.ref<@B, mut>, f32
  }
}
