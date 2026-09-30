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

// The lookup checks that the id's slot is in use and its generation
// current, finds the entity's archetype and row in the entity table, and
// loads the field from whichever archetype holds P. Every load is guarded.
// CHECK-LABEL: func.func private @chase(
// CHECK:      %[[ID:.*]] = memref.load %{{.*}} : memref<10xi64>
// CHECK:      %[[SLOTBITS:.*]] = arith.andi %[[ID]], %{{.*}} : i64
// CHECK-NEXT: %[[SLOT:.*]] = arith.index_castui %[[SLOTBITS]] : i64 to index
// CHECK:      %[[HIGH:.*]] = arith.shrui %[[ID]], %{{.*}} : i64
// CHECK-NEXT: %[[GEN:.*]] = arith.trunci %[[HIGH]] : i64 to i32
// CHECK:      %[[USED:.*]] = arith.index_cast
// CHECK-NEXT: %[[IN:.*]] = arith.cmpi ult, %[[SLOT]], %[[USED]] : index
// CHECK-NEXT: %[[R:.*]]:2 = scf.if %[[IN]] -> (f32, i1) {
// CHECK:        %[[CUR:.*]] = memref.load %{{.*}}[%[[SLOT]]]
// CHECK-NEXT:   %[[ALIVE:.*]] = arith.cmpi eq, %[[CUR]], %[[GEN]] : i32
// CHECK-NEXT:   scf.if %[[ALIVE]] -> (f32, i1) {
// The location packs archetype << 4 | row (capacity 10 needs 4 row bits).
// CHECK:          %[[PACKED:.*]] = memref.load %{{.*}}[%[[SLOT]]] : memref<20xi32>
// CHECK:          %[[WHERE:.*]] = arith.shrui %[[PACKED]], %{{.*}} : i32
// CHECK:          arith.andi %[[PACKED]], %{{.*}} : i32
// CHECK:          %[[PLAIN:.*]] = arith.constant 0 : i32
// CHECK-NEXT:     arith.cmpi eq, %[[WHERE]], %[[PLAIN]] : i32
// CHECK:            memref.load %{{.*}} : memref<10xf32>
// CHECK:            %[[TAGGED:.*]] = arith.constant 1 : i32
// CHECK-NEXT:       arith.cmpi eq, %[[WHERE]], %[[TAGGED]] : i32
// CHECK:              memref.load %{{.*}} : memref<10xf32>
// CHECK:              scf.yield %{{.*}}, %{{.*}} : f32, i1
// An entity in neither archetype, a dead id or an unused slot finds
// nothing: 0 and false.
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
