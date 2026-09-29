// RUN: ecs-opt %s --ecs-lower-to-loops -verify-diagnostics | FileCheck %s

ecs.component @A (a: f32)
ecs.component @B (b0: i32, b1: f32)
ecs.component @Unused (u: f32)

// World layout: 2 counts (16 bytes), then per column a 64-byte boundary
// plus 17 cache lines (1088 bytes):
//   AB.A.a  at 64 + 1088            = 1152  (1000 x f32)
//   AB.B.b0 at 5184 + 1088          = 6272  (1000 x i32)
//   AB.B.b1 at 10304 + 1088         = 11392 (1000 x f32)
//   OnlyA.A.a at 15424 + 1088       = 16512 (1000 x f32)
// and the arena rounds up to 16 KiB: 32768 bytes.
ecs.archetype @AB (@A, @B) capacity 1000
ecs.archetype @OnlyA (@A) capacity 1000

// CHECK-LABEL: func.func private @scale(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[W:[^:]*]]: memref<32768xi8>)
// Counts and column views are created once, at the entry.
// CHECK:      %[[COUNTS:.*]] = memref.view %[[W]][%{{.*}}][] : memref<32768xi8> to memref<2xi64>
// CHECK:      %[[N_AB_I64:.*]] = memref.load %[[COUNTS]][%{{.*}}] : memref<2xi64>
// CHECK:      %[[N_AB:.*]] = arith.index_cast %[[N_AB_I64]] : i64 to index
// CHECK:      %[[OFF_AB_A:.*]] = arith.constant 1152 : index
// CHECK:      %[[AB_A:.*]] = memref.view %[[W]][%[[OFF_AB_A]]][] : memref<32768xi8> to memref<1000xf32>
// CHECK:      %[[N_A:.*]] = arith.index_cast
// CHECK:      arith.constant 16512 : index
// CHECK-NEXT: %[[A_A:.*]] = memref.view {{.*}} to memref<1000xf32>
// CHECK:      arith.constant 6272 : index
// CHECK-NEXT: %[[AB_B0:.*]] = memref.view {{.*}} to memref<1000xi32>
// CHECK:      arith.constant 11392 : index
// CHECK-NEXT: %[[AB_B1:.*]] = memref.view {{.*}} to memref<1000xf32>
ecs.system @scale(%k: f32) reads [@B] writes [@A] {
  // Matches both archetypes: one loop each, in declaration order.
  // CHECK: scf.for %[[I:.*]] = %{{.*}} to %[[N_AB]]
  // CHECK:   %[[X:.*]] = memref.load %[[AB_A]][%[[I]]]
  // CHECK:   %[[Y:.*]] = arith.mulf %[[X]], %[[K]]
  // CHECK:   memref.store %[[Y]], %[[AB_A]][%[[I]]]
  // CHECK: scf.for %[[J:.*]] = %{{.*}} to %[[N_A]]
  // CHECK:   memref.load %[[A_A]][%[[J]]]
  // CHECK:   memref.store %{{.*}}, %[[A_A]][%[[J]]]
  ecs.query (%a: !ecs.ref<@A, mut>) {
    %x = ecs.get %a "a" : !ecs.ref<@A, mut> -> f32
    %y = arith.mulf %x, %k : f32
    ecs.set %a "a", %y : !ecs.ref<@A, mut>, f32
  }
  // Matches only @AB; accesses nested in regions are lowered as well.
  // CHECK: scf.for %[[I:.*]] = %{{.*}} to %[[N_AB]]
  // CHECK:   memref.load %[[AB_B0]][%[[I]]]
  // CHECK:   scf.if
  // CHECK:     memref.load %[[AB_B1]][%[[I]]]
  // CHECK:     memref.store %{{.*}}, %[[AB_A]][%[[I]]]
  // CHECK-NOT: scf.for
  // CHECK: return
  ecs.query (%a: !ecs.ref<@A, mut>, %b: !ecs.ref<@B>) {
    %flag = ecs.get %b "b0" : !ecs.ref<@B> -> i32
    %zero = arith.constant 0 : i32
    %set = arith.cmpi ne, %flag, %zero : i32
    scf.if %set {
      %v = ecs.get %b "b1" : !ecs.ref<@B> -> f32
      ecs.set %a "a", %v : !ecs.ref<@A, mut>, f32
    }
  }
}

// A function that touches no archetype gets no views.
// CHECK-LABEL: func.func private @dead(
// CHECK-SAME: %{{[^:]*}}: memref<32768xi8>)
// CHECK-NEXT: return
ecs.system @dead() reads [@Unused] {
  // expected-warning @+1 {{matches no archetype; the query is removed}}
  ecs.query (%u: !ecs.ref<@Unused>) {
    %v = ecs.get %u "u" : !ecs.ref<@Unused> -> f32
  }
}

// CHECK-LABEL: func.func @tick(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[W:[^:]*]]: memref<32768xi8>)
// CHECK-SAME: attributes {llvm.emit_c_interface}
// CHECK-NEXT: call @scale(%[[K]], %[[W]])
// CHECK-NEXT: call @dead(%[[W]])
ecs.schedule @tick(%k: f32) {
  ecs.run @scale(%k) : f32
  ecs.run @dead()
}

// CHECK-NOT: ecs.
