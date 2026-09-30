// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ecs-opt %s --ecs-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @P (x: f32)
ecs.component @T (entity: !ecs.entity)
ecs.component @S (s: f32)
ecs.archetype @Plain (@P) capacity 10
ecs.archetype @Tagged (@P, optional @S, @T) capacity 10

// Nothing here is despawned or moved, so an id is `archetype << 4 | row`
// (capacity 10 needs 4 row bits): the lookup decodes it, picks the
// archetype holding P, and checks the row against that archetype's count.
// Every load is guarded. (lower-lookup-generational.mlir covers ids that
// go through the entity table.)
// CHECK-LABEL: func.func private @chase(
// CHECK:      %[[ID:.*]] = memref.load %{{.*}} : memref<10xi32>
// CHECK:      %[[WHERE:.*]] = arith.shrui %[[ID]], %{{.*}} : i32
// CHECK:      %[[ROWBITS:.*]] = arith.andi %[[ID]], %{{.*}} : i32
// CHECK-NEXT: %[[ROW:.*]] = arith.index_castui %[[ROWBITS]] : i32 to index
// CHECK:      %[[PLAIN:.*]] = arith.constant 0 : i32
// CHECK-NEXT: %[[IS_PLAIN:.*]] = arith.cmpi eq, %[[WHERE]], %[[PLAIN]] : i32
// CHECK-NEXT: %[[R:.*]]:2 = scf.if %[[IS_PLAIN]] -> (f32, i1) {
// CHECK:        %[[COUNT:.*]] = arith.index_cast
// CHECK-NEXT:   %[[LIVE:.*]] = arith.cmpi ult, %[[ROW]], %[[COUNT]] : index
// CHECK-NEXT:   scf.if %[[LIVE]] -> (f32, i1) {
// CHECK-NEXT:     memref.load %{{.*}}[%[[ROW]]] : memref<10xf32>
// CHECK:      } else {
// CHECK:        %[[TAGGED:.*]] = arith.constant 1 : i32
// CHECK-NEXT:   arith.cmpi eq, %[[WHERE]], %[[TAGGED]] : i32
// An id of neither archetype, or past its count, finds nothing.
// CHECK:              %[[ZERO:.*]] = arith.constant 0.000000e+00 : f32
// CHECK-NEXT:         %[[NO:.*]] = arith.constant false
// CHECK-NEXT:         scf.yield %[[ZERO]], %[[NO]] : f32, i1
// The query binds the optional S, so its store is masked.
// CHECK:      %[[OLD:.*]] = memref.load
// CHECK-NEXT: arith.select %{{.*}}, %[[R]]#0, %[[OLD]] : f32
ecs.system @chase() reads [@P, @T] writes [@S] {
  ecs.query (%t: !ecs.ref<@T>, %s: !ecs.ref<@S, mut>) {
    %target = ecs.get %t "entity" : !ecs.ref<@T> -> !ecs.entity
    %x, %found = ecs.lookup %target @P "x" : f32
    ecs.set %s "s", %x : !ecs.ref<@S, mut>, f32
  }
}

// The query changes nothing its lookup reads, so it may run in parallel,
// and every load in the lookup is guarded, so its optional binding stays
// branch-free (masked stores).
// PAR-LABEL: func.func private @chase(
// PAR:       scf.parallel
// PAR:         arith.select

ecs.system @shift(%d: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}

// Fused per entity, a lookup would see the shift of some targets and not
// others; a system with lookups stays a call between fused sequences.
// FUSED-LABEL: func.func @frame(
// FUSED:       scf.for
// FUSED:       scf.for
// FUSED:       call @chase(
// FUSED-NEXT:  arith.constant
// FUSED:       scf.for
// FUSED:       return
ecs.schedule @frame(%d: f32) {
  ecs.run @shift(%d) : f32
  ecs.run @chase()
  ecs.run @shift(%d) : f32
}
