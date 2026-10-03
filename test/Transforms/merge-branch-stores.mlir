// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s

ent.component @N (v: f32, n: i32) capacity 100
ent.archetype @A (@N) capacity 100

// Both branches set v and n: the `if` yields the values and each column is
// stored once after it, so the branch can become a select.
// CHECK-LABEL: func.func private @clamp(
// CHECK:        %[[R:.*]]:2 = scf.if %{{.*}} -> (f32, i32) {
// CHECK-NOT:      memref.store
// CHECK:          scf.yield %{{.*}}, %{{.*}} : f32, i32
// CHECK-NEXT:   } else {
// CHECK-NOT:      memref.store
// CHECK:          scf.yield %{{.*}}, %{{.*}} : f32, i32
// CHECK-NEXT:   }
// CHECK-NEXT:   memref.store %[[R]]#0, %{{.*}}[%[[ROW:.*]]] : memref<100xf32>
// CHECK-NEXT:   memref.store %[[R]]#1, %{{.*}}[%[[ROW]]] : memref<100xi32>
ent.system @clamp() {
  ent.query (%x: !ent.ref<@N, mut>) {
    %v = ent.get %x "v" : !ent.ref<@N, mut> -> f32
    %one = arith.constant 1.0 : f32
    %over = arith.cmpf oge, %v, %one : f32
    scf.if %over {
      %z = arith.constant 0.0 : f32
      %c = arith.constant 1 : i32
      ent.set %x "v", %z : !ent.ref<@N, mut>, f32
      ent.set %x "n", %c : !ent.ref<@N, mut>, i32
    } else {
      %c = arith.constant 0 : i32
      ent.set %x "v", %v : !ent.ref<@N, mut>, f32
      ent.set %x "n", %c : !ent.ref<@N, mut>, i32
    }
  }
}

// Only one branch sets n: that store stays in its branch; v is merged.
// CHECK-LABEL: func.func private @partial(
// CHECK:        %[[R:.*]] = scf.if %{{.*}} -> (f32) {
// CHECK:          memref.store %{{.*}} : memref<100xi32>
// CHECK:        } else {
// CHECK-NOT:      memref.store
// CHECK:        }
// CHECK-NEXT:   memref.store %[[R]], %{{.*}} : memref<100xf32>
ent.system @partial() {
  ent.query (%x: !ent.ref<@N, mut>) {
    %v = ent.get %x "v" : !ent.ref<@N, mut> -> f32
    %one = arith.constant 1.0 : f32
    %over = arith.cmpf oge, %v, %one : f32
    scf.if %over {
      %z = arith.constant 0.0 : f32
      %c = arith.constant 1 : i32
      ent.set %x "v", %z : !ent.ref<@N, mut>, f32
      ent.set %x "n", %c : !ent.ref<@N, mut>, i32
    } else {
      ent.set %x "v", %v : !ent.ref<@N, mut>, f32
    }
  }
}
