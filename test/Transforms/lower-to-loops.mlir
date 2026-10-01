// RUN: ent-opt %s --ent-lower-to-loops -verify-diagnostics | FileCheck %s

ent.component @A (a: f32)
ent.component @B (b0: i32, b1: f32)
ent.component @Unused (u: f32)

// World layout: 2 counts and the entity counters (32 bytes), then per
// column a 64-byte boundary plus 17 cache lines (1088 bytes):
//   AB.A.a    at 64 + 1088     = 1152  (1000 x f32)
//   AB.B.b0   at 5184 + 1088   = 6272  (1000 x i32)
//   AB.B.b1   at 10304 + 1088  = 11392 (1000 x f32)
//   OnlyA.A.a at 15424 + 1088  = 16512 (1000 x f32)
// Nothing is despawned or moved, so ids are just archetype and row: no id
// columns and no entity table. The arena rounds up to 16 KiB: 32768 bytes.
ent.archetype @AB (@A, @B) capacity 1000
ent.archetype @OnlyA (@A) capacity 1000

// CHECK-LABEL: func.func private @scale(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[W:[^:]*]]: memref<32768xi8>)
// Views are created once, at the entry. Counts are loaded when the query
// starts, for every archetype it matches: it visits the entities that exist
// then, though spawns and despawns change the counts.
// CHECK:      %[[COUNTS:.*]] = memref.view %[[W]][%{{.*}}][] : memref<32768xi8> to memref<2xi64>
// CHECK:      %[[OFF_AB_A:.*]] = arith.constant 1152 : index
// CHECK-NEXT: %[[AB_A:.*]] = memref.view %[[W]][%[[OFF_AB_A]]][] : memref<32768xi8> to memref<1000xf32>
// CHECK:      arith.constant 16512 : index
// CHECK-NEXT: %[[A_A:.*]] = memref.view {{.*}} to memref<1000xf32>
// CHECK:      arith.constant 6272 : index
// CHECK-NEXT: %[[AB_B0:.*]] = memref.view {{.*}} to memref<1000xi32>
// CHECK:      arith.constant 11392 : index
// CHECK-NEXT: %[[AB_B1:.*]] = memref.view {{.*}} to memref<1000xf32>
ent.system @scale(%k: f32) reads [@B] writes [@A] {
  // Matches both archetypes: one loop each, in declaration order, both
  // bounded by the counts taken when the query starts.
  // CHECK:      %[[P_AB:.*]] = arith.constant 0 : index
  // CHECK-NEXT: %[[N_AB_I64:.*]] = memref.load %[[COUNTS]][%[[P_AB]]] : memref<2xi64>
  // CHECK-NEXT: %[[N_AB:.*]] = arith.index_cast %[[N_AB_I64]] : i64 to index
  // CHECK-NEXT: %[[P_A:.*]] = arith.constant 1 : index
  // CHECK-NEXT: %[[N_A_I64:.*]] = memref.load %[[COUNTS]][%[[P_A]]] : memref<2xi64>
  // CHECK-NEXT: %[[N_A:.*]] = arith.index_cast %[[N_A_I64]] : i64 to index
  // CHECK:      scf.for %[[I:.*]] = %{{.*}} to %[[N_AB]]
  // CHECK:   %[[X:.*]] = memref.load %[[AB_A]][%[[I]]]
  // CHECK:   %[[Y:.*]] = arith.mulf %[[X]], %[[K]]
  // CHECK:   memref.store %[[Y]], %[[AB_A]][%[[I]]]
  // CHECK:      scf.for %[[J:.*]] = %{{.*}} to %[[N_A]]
  // CHECK:   memref.load %[[A_A]][%[[J]]]
  // CHECK:   memref.store %{{.*}}, %[[A_A]][%[[J]]]
  ent.query (%a: !ent.ref<@A, mut>) {
    %x = ent.get %a "a" : !ent.ref<@A, mut> -> f32
    %y = arith.mulf %x, %k : f32
    ent.set %a "a", %y : !ent.ref<@A, mut>, f32
  }
  // Matches only @AB; accesses nested in regions are lowered as well.
  // CHECK:      memref.load %[[COUNTS]]
  // CHECK-NEXT: %[[N_AB2:.*]] = arith.index_cast
  // CHECK:      scf.for %[[I:.*]] = %{{.*}} to %[[N_AB2]]
  // CHECK:   memref.load %[[AB_B0]][%[[I]]]
  // CHECK:   scf.if
  // CHECK:     memref.load %[[AB_B1]][%[[I]]]
  // CHECK:     memref.store %{{.*}}, %[[AB_A]][%[[I]]]
  // CHECK-NOT: scf.for
  // CHECK: return
  ent.query (%a: !ent.ref<@A, mut>, %b: !ent.ref<@B>) {
    %flag = ent.get %b "b0" : !ent.ref<@B> -> i32
    %zero = arith.constant 0 : i32
    %set = arith.cmpi ne, %flag, %zero : i32
    scf.if %set {
      %v = ent.get %b "b1" : !ent.ref<@B> -> f32
      ent.set %a "a", %v : !ent.ref<@A, mut>, f32
    }
  }
}

// A function that touches no archetype gets no views.
// CHECK-LABEL: func.func private @dead(
// CHECK-SAME: %{{[^:]*}}: memref<32768xi8>)
// CHECK-NEXT: return
ent.system @dead() reads [@Unused] {
  // expected-warning @+1 {{matches no archetype; the query is removed}}
  ent.query (%u: !ent.ref<@Unused>) {
    %v = ent.get %u "u" : !ent.ref<@Unused> -> f32
  }
}

// CHECK-LABEL: func.func @tick(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[W:[^:]*]]: memref<32768xi8>)
// CHECK-SAME: attributes {llvm.emit_c_interface}
// CHECK-NEXT: call @scale(%[[K]], %[[W]])
// CHECK-NEXT: call @dead(%[[W]])
ent.schedule @tick(%k: f32) {
  ent.run @scale(%k) : f32
  ent.run @dead()
}

// CHECK-NOT: ent.
