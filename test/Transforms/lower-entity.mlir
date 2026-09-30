// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s

// !ecs.entity is lowered to the integer the layout stores ids as. Nothing
// here is despawned or moved, so an id is `archetype << rowBits | row` in
// 32 bits, computed rather than stored. Ids stored in fields, passed as
// parameters, yielded by scf.if and selected by arith.select all become
// that integer; no !ecs.entity and no cast is left.
ecs.component @P (x: f32)
ecs.component @Link (to: !ecs.entity)
ecs.archetype @Node (@P, @Link) capacity 10

// CHECK-NOT: !ecs.entity
// CHECK-NOT: unrealized_conversion_cast

// An entity parameter becomes an integer parameter.
// CHECK-LABEL: func.func private @point(
// CHECK-SAME: %[[TARGET:[^:]*]]: i32, %{{[^:]*}}: memref<{{.*}}xi8>)
ecs.system @point(%target: !ecs.entity) reads [@P] writes [@Link] {
  ecs.query (%l: !ecs.ref<@Link, mut>, %p: !ecs.ref<@P>) {
    // An id loaded from the id column, chosen against a parameter and
    // stored into an entity field.
    // CHECK:      scf.for %[[I:.*]] =
    // CHECK-NEXT:   %[[ARCHETYPE:.*]] = arith.constant 0 : i32
    // CHECK-NEXT:   %[[ROW:.*]] = arith.index_castui %[[I]] : index to i32
    // CHECK-NEXT:   %[[SELF:.*]] = arith.ori %[[ARCHETYPE]], %[[ROW]] : i32
    // CHECK:        %[[PICK:.*]] = scf.if %{{.*}} -> (i32) {
    // CHECK-NEXT:     scf.yield %[[SELF]] : i32
    // CHECK-NEXT:   } else {
    // CHECK-NEXT:     scf.yield %[[TARGET]] : i32
    // CHECK-NEXT:   }
    // CHECK:        %[[CHOSEN:.*]] = arith.select %{{.*}}, %[[PICK]], %[[TARGET]] : i32
    // CHECK-NEXT:   memref.store %[[CHOSEN]], %{{.*}}[%[[I]]] : memref<10xi32>
    %self = ecs.entity
    %x = ecs.get %p "x" : !ecs.ref<@P> -> f32
    %zero = arith.constant 0.0 : f32
    %neg = arith.cmpf olt, %x, %zero : f32
    %pick = scf.if %neg -> !ecs.entity {
      scf.yield %self : !ecs.entity
    } else {
      scf.yield %target : !ecs.entity
    }
    %big = arith.cmpf ogt, %x, %zero : f32
    %chosen = arith.select %big, %pick, %target : !ecs.entity
    ecs.set %l "to", %chosen : !ecs.ref<@Link, mut>, !ecs.entity
  }
}

// A schedule passes an entity parameter on to the system.
// CHECK-LABEL: func.func @frame(
// CHECK-SAME: %[[T:[^:]*]]: i32,
// CHECK: call @point(%[[T]], %{{.*}}) : (i32, memref<{{.*}}xi8>) -> ()
ecs.schedule @frame(%t: !ecs.entity) {
  ecs.run @point(%t) : !ecs.entity
}
