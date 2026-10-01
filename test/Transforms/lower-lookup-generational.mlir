// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s

ent.component @P (x: f32)
ent.component @T (entity: !ent.entity)
ent.archetype @Plain (@P) capacity 10
ent.archetype @Tagged (@P, @T) capacity 10

// Plain entities are despawned, so ids are generational: 20 slots need 5
// bits, which leaves 27 generation bits in a 32-bit id. A lookup checks
// that the slot is in use and its generation current, then reads the
// entity's packed location (archetype << 4 | row) from the entity table.
// CHECK-LABEL: func.func private @chase(
// CHECK:      %[[ID:.*]] = memref.load %{{.*}} : memref<10xi32>
// CHECK:      %[[SLOTBITS:.*]] = arith.andi %[[ID]], %{{.*}} : i32
// CHECK-NEXT: %[[SLOT:.*]] = arith.index_castui %[[SLOTBITS]] : i32 to index
// CHECK:      %[[USED:.*]] = arith.index_cast
// CHECK-NEXT: %[[IN:.*]] = arith.cmpi ult, %[[SLOT]], %[[USED]] : index
// CHECK-NEXT: %[[R:.*]]:2 = scf.if %[[IN]] -> (f32, i1) {
// CHECK:        %[[CUR:.*]] = memref.load %{{.*}}[%[[SLOT]]] : memref<20xi32>
// The generation fills the 27 bits above the slot: no mask needed.
// CHECK-NEXT:   %[[FIVE:.*]] = arith.constant 5 : i32
// CHECK-NEXT:   %[[GEN:.*]] = arith.shrui %[[ID]], %[[FIVE]] : i32
// CHECK-NEXT:   %[[ALIVE:.*]] = arith.cmpi eq, %[[CUR]], %[[GEN]] : i32
// CHECK-NEXT:   scf.if %[[ALIVE]] -> (f32, i1) {
// CHECK:          %[[PACKED:.*]] = memref.load %{{.*}}[%[[SLOT]]] : memref<20xi32>
// CHECK:          %[[WHERE:.*]] = arith.shrui %[[PACKED]], %{{.*}} : i32
// CHECK:          arith.andi %[[PACKED]], %{{.*}} : i32
// CHECK:          arith.cmpi eq, %[[WHERE]], %{{.*}} : i32
ent.system @chase() reads [@P, @T] {
  ent.query (%t: !ent.ref<@T>) {
    %target = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    %x, %found = ent.lookup %target @P "x" : f32
  }
}

ent.system @cull() reads [@P] writes [@Plain, @Tagged] {
  ent.query (%p: !ent.ref<@P>) {
    ent.despawn
  }
}
