// RUN: ecs-opt %s --ecs-lower-to-loops --canonicalize | FileCheck %s
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ecs-opt %s "--ecs-lower-to-loops=fuse-systems=1" --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @H (hp: f32, lvl: i32)
ecs.component @T (entity: !ecs.entity)
ecs.component @On ()
ecs.archetype @Ship (@H) capacity 16
ecs.archetype @Gun (@T, optional @On) capacity 8

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
// CHECK:      scf.for %[[ROW2:.*]] = %{{.*}} to %{{.*}} step
// CHECK-NEXT:   %[[SENT_ID:.*]] = memref.load %[[IDS]][%[[ROW2]]]
// CHECK-NEXT:   %[[SENT:.*]] = arith.cmpi ne, %[[SENT_ID]], %c-1_i32
// CHECK-NEXT:   scf.if %[[SENT]] {
// CHECK-NEXT:     %[[V:.*]] = memref.load %[[VALUES]][%[[ROW2]]]
// CHECK:          %[[OLD:.*]] = memref.load %[[HP:.*]][%[[AT:.*]]] : memref<16xf32>
// CHECK-NEXT:     %[[NEW:.*]] = arith.addf %[[OLD]], %[[V]] : f32
// CHECK-NEXT:     memref.store %[[NEW]], %[[HP]][%[[AT]]] : memref<16xf32>
// The pending despawns come after.
// CHECK:      scf.for
// CHECK:        arith.subi
// CHECK:      return
ecs.system @fire(%d: f32) reads [@T] writes [@H, @Gun] {
  ecs.query (%t: !ecs.ref<@T>) {
    %id = ecs.get %t "entity" : !ecs.ref<@T> -> !ecs.entity
    ecs.apply %id @H "hp" add %d : f32
    ecs.despawn
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
ecs.system @aim(%n: i32, %c: i1) reads [@T, @On] writes [@H] {
  ecs.query (%t: !ecs.ref<@T>, %on: !ecs.ref<@On>) {
    %id = ecs.get %t "entity" : !ecs.ref<@T> -> !ecs.entity
    ecs.apply %id @H "lvl" max %n : i32
    scf.if %c {
      ecs.apply %id @H "lvl" max %n : i32
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
ecs.system @read() reads [@H] {
  ecs.query (%h: !ecs.ref<@H>) {
    %l = ecs.get %h "lvl" : !ecs.ref<@H> -> i32
  }
}
// FUSED-LABEL: func.func @frame(
// FUSED:       call @aim(
// FUSED-NOT:   call @read(
// FUSED:       scf.for
// FUSED:       return
ecs.schedule @frame(%n: i32, %c: i1) {
  ecs.run @aim(%n, %c) : i32, i1
  ecs.run @read()
}
