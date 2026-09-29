// RUN: ecs-opt %s --ecs-lower-to-loops -verify-diagnostics | FileCheck %s

ecs.component @A (a: f32)
ecs.component @B (b0: i32, b1: f32)
ecs.component @Unused (u: f32)

// World ABI: per archetype a count, then one column per field.
ecs.archetype @AB (@A, @B) capacity 1000
ecs.archetype @OnlyA (@A) capacity 1000

// CHECK-LABEL: func.func private @scale(
// CHECK-SAME: %[[K:[^:]*]]: f32,
// CHECK-SAME: %[[N_AB:[^:]*]]: index, %[[AB_A:[^:]*]]: memref<?xf32>, %[[AB_B0:[^:]*]]: memref<?xi32>, %[[AB_B1:[^:]*]]: memref<?xf32>,
// CHECK-SAME: %[[N_A:[^:]*]]: index, %[[A_A:[^:]*]]: memref<?xf32>)
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

// CHECK-LABEL: func.func private @dead(
// CHECK-NOT: scf.for
// CHECK: return
ecs.system @dead() reads [@Unused] {
  // expected-warning @+1 {{matches no archetype; the query is removed}}
  ecs.query (%u: !ecs.ref<@Unused>) {
    %v = ecs.get %u "u" : !ecs.ref<@Unused> -> f32
  }
}

// CHECK-LABEL: func.func @tick(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[W0:[^:]*]]: index, %[[W1:[^:]*]]: memref<?xf32>, %[[W2:[^:]*]]: memref<?xi32>, %[[W3:[^:]*]]: memref<?xf32>, %[[W4:[^:]*]]: index, %[[W5:[^:]*]]: memref<?xf32>)
// CHECK-SAME: attributes {llvm.emit_c_interface}
// CHECK: call @scale(%[[K]], %[[W0]], %[[W1]], %[[W2]], %[[W3]], %[[W4]], %[[W5]])
// CHECK: call @dead(%[[W0]], %[[W1]], %[[W2]], %[[W3]], %[[W4]], %[[W5]])
ecs.schedule @tick(%k: f32) {
  ecs.run @scale(%k) : f32
  ecs.run @dead()
}

// CHECK-NOT: ecs.
