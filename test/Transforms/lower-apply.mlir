// RUN: ent-opt %s --ent-lower-to-loops=direct-applies=0 --canonicalize \
// RUN:   | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops --canonicalize \
// RUN:   | FileCheck %s --check-prefix=DIRECT
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ent-opt %s "--ent-lower-to-loops=fuse-systems=1" --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @H (hp: f32, lvl: i32)
ent.component @T (entity: !ent.entity)
ent.component @On ()
ent.archetype @Ship (@H) capacity 16
ent.archetype @Gun (@T, optional @On) capacity 8

// Each gun fills its row of the apply's buffer (target id, value). When
// the query has run, one sequential loop goes over the buffer in row
// order, skips rows that sent nothing (the all-ones id), finds the target
// like a lookup, and combines the value into its field. Then the
// despawns are applied.
// CHECK-LABEL: func.func private @fire(
// CHECK:      scf.for %[[ROW:.*]] = %{{.*}} to %{{.*}} step
// CHECK:        %[[ID:.*]] = memref.load %[[GUN_T:.*]][%[[ROW]]] : memref<8xi32>
// CHECK-NEXT:   memref.store %[[ID]], %[[IDS:.*]][%[[ROW]]] : memref<8xi32>
// CHECK-NEXT:   memref.store %{{.*}}, %[[VALUES:.*]][%[[ROW]]] : memref<8xf32>
// Ids here are generational (the system despawns); the slots in use that
// they are checked against cannot change while combining, so they are
// loaded once, before the loop (after the despawns are listed).
// CHECK:      memref.store %{{.*}}, %{{.*}}[%c0] : memref<1xi64>
// CHECK-NEXT: }
// CHECK-NEXT: %[[USED64:.*]] = memref.load %{{.*}}[%c0] : memref<1xi64>
// CHECK-NEXT: %[[USED:.*]] = arith.index_cast %[[USED64]] : i64 to index
// CHECK-NEXT: scf.for %[[ROW2:.*]] = %{{.*}} to %{{.*}} step
// CHECK-NEXT:   %[[SENT_ID:.*]] = memref.load %[[IDS]][%[[ROW2]]]
// CHECK-NEXT:   %[[SENT:.*]] = arith.cmpi ne, %[[SENT_ID]], %c-1_i32
// CHECK-NEXT:   scf.if %[[SENT]] {
// CHECK-NEXT:     %[[V:.*]] = memref.load %[[VALUES]][%[[ROW2]]]
// CHECK:          arith.cmpi ult, %{{.*}}, %[[USED]] : index
// CHECK:          %[[OLD:.*]] = memref.load %[[HP:.*]][%[[AT:.*]]] : memref<16xf32>
// CHECK-NEXT:     %[[NEW:.*]] = arith.addf %[[OLD]], %[[V]] : f32
// CHECK-NEXT:     memref.store %[[NEW]], %[[HP]][%[[AT]]] : memref<16xf32>
// The pending despawns come after.
// CHECK:      scf.for
// CHECK:        arith.subi
// CHECK:      return
// In a loop that never runs in parallel, an apply whose field nothing else
// in the query touches is combined as the loop visits the guns: same order,
// no buffer, no second loop.
// DIRECT-LABEL: func.func private @fire(
// DIRECT:      scf.for %[[ROW:.*]] = %{{.*}} to %{{.*}} step
// DIRECT:        %[[ID:.*]] = memref.load %{{.*}}[%[[ROW]]] : memref<8xi32>
// DIRECT-NOT:    memref.store %[[ID]]
// DIRECT:        %[[OLD:.*]] = memref.load %[[HP:.*]][%[[AT:.*]]] : memref<16xf32>
// DIRECT-NEXT:   %[[NEW:.*]] = arith.addf %[[OLD]], %{{.*}} : f32
// DIRECT-NEXT:   memref.store %[[NEW]], %[[HP]][%[[AT]]] : memref<16xf32>
// Two applies to one field are combined by apply first: buffered.
// DIRECT-LABEL: func.func private @aim(
// DIRECT:      memref.store %c-1_i32
ent.system @fire(%d: f32) reads [@T] writes [@H, @Gun] {
  ent.query (%t: !ent.ref<@T>) {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
    ent.despawn
  }
}

// Binding the optional On masks the body: a gun without it sends nothing
// (the id is selected against the all-ones id). An apply under an `if`
// may not run, so its slot is set to "no target" first. Applies are
// combined in program order, integers with signed max.
// CHECK-LABEL: func.func private @aim(
// CHECK:      scf.for %[[ROW:.*]] = %{{.*}} to %{{.*}} step
// CHECK:        %[[ON:.*]] = arith.cmpi ne, %{{.*}}, %c0_i8 : i8
// CHECK-NEXT:   memref.store %c-1_i32, %[[IDS1:.*]][%[[ROW]]] : memref<8xi32>
// CHECK-NEXT:   %[[ID:.*]] = memref.load
// CHECK-NEXT:   %[[MASKED:.*]] = arith.select %[[ON]], %[[ID]], %c-1_i32 : i32
// CHECK-NEXT:   memref.store %[[MASKED]], %[[IDS0:.*]][%[[ROW]]] : memref<8xi32>
// CHECK-NEXT:   memref.store
// CHECK-NEXT:   scf.if %{{.*}} {
// CHECK-NEXT:     %[[MASKED1:.*]] = arith.select %[[ON]], %[[ID]], %c-1_i32 : i32
// CHECK-NEXT:     memref.store %[[MASKED1]], %[[IDS1]][%[[ROW]]] : memref<8xi32>
// CHECK:      scf.for
// CHECK-NEXT:   memref.load %[[IDS0]]
// CHECK:          arith.maxsi
// CHECK:      scf.for
// CHECK-NEXT:   memref.load %[[IDS1]]
// CHECK:          arith.maxsi
ent.system @aim(%n: i32, %c: i1) reads [@T, @On] writes [@H] {
  ent.query (%t: !ent.ref<@T>, %on: !ent.ref<@On>) {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "lvl" max %n : i32
    scf.if %c {
      ent.apply %id @H "lvl" max %n : i32
    }
  }
}

// The query filling the buffers is entity-local, so it may run in
// parallel; the combining loop stays sequential.
// PAR-LABEL: func.func private @aim(
// PAR:       scf.parallel
// PAR:       scf.for
// PAR:         arith.maxsi

// A system with applies is not fused with its neighbours: the reader of
// "lvl" must see the combined values.
ent.system @read() reads [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %l = ent.get %h "lvl" : !ent.ref<@H> -> i32
  }
}
// FUSED-LABEL: func.func @frame(
// FUSED:       call @aim(
// FUSED-NOT:   call @read(
// FUSED:       scf.for
// FUSED:       return
ent.schedule @frame(%n: i32, %c: i1) {
  ent.run @aim(%n, %c) : i32, i1
  ent.run @read()
}

// An accumulate nothing else in its query reads: buffered per row and
// summed after the loop, or (direct) added up as the loop visits the guns,
// the sum carried through the loop and stored once after it.
ent.resource @Score (points: i64)
// CHECK-LABEL: func.func private @tally(
// CHECK:      scf.for %[[ROW:.*]] =
// CHECK:        memref.store %{{.*}}, %{{.*}}[%[[ROW]]] : memref<8xi64>
// CHECK:      scf.for
// DIRECT-LABEL: func.func private @tally(
// DIRECT:      %[[OLD:.*]] = memref.load %[[SCORE:.*]][%c0] : memref<1xi64>
// DIRECT-NEXT: %[[R:.*]] = scf.for %{{.*}} = {{.*}} iter_args(%[[SUM:.*]] = %[[OLD]]) -> (i64) {
// DIRECT-NEXT:   %[[NEW:.*]] = arith.addi %[[SUM]], %{{.*}} : i64
// DIRECT-NEXT:   scf.yield %[[NEW]] : i64
// DIRECT-NEXT: }
// DIRECT-NEXT: memref.store %[[R]], %[[SCORE]][%c0] : memref<1xi64>
ent.system @tally() reads [@T] writes [@Score] {
  ent.query (%t: !ent.ref<@T>) {
    %one = arith.constant 1 : i64
    ent.accumulate @Score "points" add %one : i64
  }
}
