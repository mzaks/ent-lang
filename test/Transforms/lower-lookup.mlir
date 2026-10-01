// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @P (x: f32)
ent.component @T (entity: !ent.entity)
ent.component @S (s: f32)
ent.archetype @Plain (@P) capacity 10
ent.archetype @Tagged (@P, optional @S, @T) capacity 10

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
ent.system @chase() reads [@P, @T] writes [@S] {
  ent.query (%t: !ent.ref<@T>, %s: !ent.ref<@S, mut>) {
    %target = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    %x, %found = ent.lookup %target @P "x" : f32
    ent.set %s "s", %x : !ent.ref<@S, mut>, f32
  }
}

// The query changes nothing its lookup reads, so it may run in parallel,
// and every load in the lookup is guarded, so its optional binding stays
// branch-free (masked stores).
// PAR-LABEL: func.func private @chase(
// PAR:       scf.parallel
// PAR:         arith.select

ent.system @shift(%d: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
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
ent.schedule @frame(%d: f32) {
  ent.run @shift(%d) : f32
  ent.run @chase()
  ent.run @shift(%d) : f32
}
