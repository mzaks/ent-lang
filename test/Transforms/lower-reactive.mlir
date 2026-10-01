// RUN: ent-opt %s --ent-lower-to-loops --canonicalize | FileCheck %s

ent.component @H (hp: f32)
ent.component @B (w: f32)
ent.archetype @A (@H, @B) capacity 8

// A write to an observed field stores the current tick (one past the
// counter, loaded once before the loop) in the field's stamp column, and
// appends the entity to the stamp's event log if its stamp moved on to
// this tick and the log still has room for its slowest reader. The log
// holds 64 entries here (the minimum), in one segment, whose count and
// slowest reader's position are the first two i64s of the counts. The
// first event to find the log full moves its count one past, so that
// readers see the overflow.
// CHECK-LABEL: func.func private @hurt(
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[HP:.*]] = memref.view %{{.*}}[%c1280][] : memref<16384xi8> to memref<8xf32>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3584][] : memref<16384xi8> to memref<8xi64>
// CHECK-DAG:  %[[COUNTS:.*]] = memref.view %{{.*}}[%c64][] : memref<16384xi8> to memref<8xi64>
// CHECK-DAG:  %[[IDS:.*]] = memref.view %{{.*}} : memref<16384xi8> to memref<64xi32>
// CHECK-DAG:  %[[TICKS:.*]] = memref.view %{{.*}} : memref<16384xi8> to memref<64xi64>
// CHECK:      %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[TICK:.*]] = arith.addi %[[C]], %c1_i64
// CHECK:      scf.for %[[ROW:.*]] =
// CHECK-NEXT:   memref.store %{{.*}}, %[[HP]][%[[ROW]]]
// CHECK-NEXT:   %[[OLD:.*]] = memref.load %[[STAMP]][%[[ROW]]]
// CHECK-NEXT:   memref.store %[[TICK]], %[[STAMP]][%[[ROW]]]
// CHECK:        %[[NEW:.*]] = arith.cmpi ne, %[[OLD]], %[[TICK]] : i64
// CHECK-NEXT:   %[[COUNT:.*]] = memref.load %[[COUNTS]][%c0]
// CHECK-NEXT:   %[[SLOWEST:.*]] = memref.load %[[COUNTS]][%c1]
// CHECK-NEXT:   %[[PENDING:.*]] = arith.subi %[[COUNT]], %[[SLOWEST]]
// CHECK-NEXT:   %[[ROOM:.*]] = arith.cmpi slt, %[[PENDING]], %c64_i64 : i64
// CHECK-NEXT:   %[[FULL:.*]] = arith.cmpi eq, %[[PENDING]], %c64_i64 : i64
// CHECK-NEXT:   %[[APPEND:.*]] = arith.andi %[[NEW]], %[[ROOM]] : i1
// CHECK-NEXT:   scf.if %[[APPEND]] {
// CHECK:          memref.store %{{.*}}, %[[IDS]][%{{.*}}]
// CHECK-NEXT:     memref.store %[[TICK]], %[[TICKS]][%{{.*}}]
// CHECK-NEXT:   }
// CHECK-NEXT:   %[[LOST:.*]] = arith.andi %[[NEW]], %[[FULL]] : i1
// CHECK-NEXT:   scf.if %[[LOST]] {
// CHECK-NEXT:     %[[PAST:.*]] = arith.addi %[[SLOWEST]], %c65_i64
// CHECK-NEXT:     memref.store %[[PAST]], %[[COUNTS]][%c0]
ent.system @hurt(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H, mut>) {
    ent.set %h "hp", %d : !ent.ref<@H, mut>, f32
  }
}

// A reactive query takes the tick it last started at, and makes the next
// one both its new last tick and the counter. It notes where the log ends
// now. On its first run (it had seen tick 0), or if more entries were
// appended since it last read the log than the log holds, it scans every
// row and keeps its stores only where the stamp is newer (branch-free,
// like a body masked by an optional component). Otherwise it walks the log
// from where it last read it to the noted end, skipping entries whose
// entity's stamp has moved on since. Either way it then records the noted
// end as its position, and the slowest reader's position.
// CHECK-LABEL: func.func private @redraw(
// CHECK-DAG:  %[[LAST:.*]] = memref.view %{{.*}}[%c32][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[COUNTS:.*]] = memref.view %{{.*}}[%c64][] : memref<16384xi8> to memref<8xi64>
// CHECK-DAG:  %[[ENDS:.*]] = memref.view %{{.*}}[%c128][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[POSITION:.*]] = memref.view %{{.*}}[%c136][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3584][] : memref<16384xi8> to memref<8xi64>
// CHECK-DAG:  %[[W:.*]] = memref.view %{{.*}}[%c2432][] : memref<16384xi8> to memref<8xf32>
// CHECK:      %[[SEEN:.*]] = memref.load %[[LAST]][%c0]
// CHECK-NEXT: %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[NEXT:.*]] = arith.addi %[[C]], %c1_i64
// CHECK-NEXT: memref.store %[[NEXT]], %[[LAST]][%c0]
// CHECK-NEXT: memref.store %[[NEXT]], %[[COUNTER]][%c0]
// CHECK-NEXT: %[[FIRST:.*]] = arith.cmpi eq, %[[SEEN]], %c0_i64
// CHECK-NEXT: %[[END:.*]] = memref.load %[[COUNTS]][%c0]
// CHECK-NEXT: memref.store %[[END]], %[[ENDS]][%c0]
// CHECK-NEXT: %[[FROM:.*]] = memref.load %[[POSITION]][%c0]
// CHECK-NEXT: %[[PENDING:.*]] = arith.subi %[[END]], %[[FROM]]
// CHECK-NEXT: %[[LOST:.*]] = arith.cmpi sgt, %[[PENDING]], %c64_i64
// CHECK-NEXT: %[[SCAN:.*]] = arith.ori %[[FIRST]], %[[LOST]]
// CHECK:      scf.if %[[SCAN]] {
// CHECK:        scf.for %[[ROW:.*]] =
// CHECK-NEXT:     %[[S:.*]] = memref.load %[[STAMP]][%[[ROW]]]
// CHECK-NEXT:     %[[NEW:.*]] = arith.cmpi sgt, %[[S]], %[[SEEN]] : i64
// CHECK:          %[[V:.*]] = arith.select %[[NEW]], %{{.*}}, %{{.*}} : f32
// CHECK-NEXT:     memref.store %[[V]], %[[W]][%[[ROW]]]
// CHECK:      } else {
// CHECK:        scf.for %{{.*}} = %{{.*}} to %{{.*}} step
// CHECK:          %[[WHEN:.*]] = memref.load %{{.*}} : memref<64xi64>
// CHECK:          %[[NOW:.*]] = memref.load %[[STAMP]][%[[AT:.*]]]
// CHECK-NEXT:     %[[LATEST:.*]] = arith.cmpi eq, %[[NOW]], %[[WHEN]] : i64
// CHECK-NEXT:     scf.if %[[LATEST]] {
// CHECK-NEXT:       %[[HP:.*]] = memref.load %{{.*}}[%[[AT]]] : memref<8xf32>
// CHECK-NEXT:       memref.store %[[HP]], %[[W]][%[[AT]]]
// CHECK:      %[[NOTED:.*]] = memref.load %[[ENDS]][%c0]
// CHECK-NEXT: memref.store %[[NOTED]], %[[POSITION]][%c0]
// CHECK-NEXT: %[[SLOWEST:.*]] = memref.load %[[POSITION]][%c0]
// CHECK-NEXT: memref.store %[[SLOWEST]], %[[COUNTS]][%c1]
ent.system @redraw() reads [@H] writes [@B] {
  ent.query (%h: !ent.ref<@H>, %b: !ent.ref<@B, mut>) on [changed @H "hp"] {
    %x = ent.get %h "hp" : !ent.ref<@H> -> f32
    ent.set %b "w", %x : !ent.ref<@B, mut>, f32
  }
}

// A spawn stamps its row: the entity has added and changed its components.
// CHECK-LABEL: func.func private @make(
// CHECK-DAG:  %[[COUNTER:.*]] = memref.view %{{.*}}[%c24][] : memref<16384xi8> to memref<1xi64>
// CHECK-DAG:  %[[STAMP:.*]] = memref.view %{{.*}}[%c3584][] : memref<16384xi8> to memref<8xi64>
// CHECK:      %[[C:.*]] = memref.load %[[COUNTER]][%c0]
// CHECK-NEXT: %[[TICK:.*]] = arith.addi %[[C]], %c1_i64
// CHECK-NEXT: memref.store %[[TICK]], %[[STAMP]][%{{.*}}] : memref<8xi64>
ent.system @make(%x: f32) writes [@A] {
  ent.spawn @A(%x, %x) : f32, f32
}
