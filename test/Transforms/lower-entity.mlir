// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s

// !ecs.entity is lowered to the integer the layout stores ids as (i64
// here). Ids stored in fields, passed as parameters, yielded by scf.if and
// selected by arith.select all become that integer; no !ecs.entity and no
// cast is left.
ecs.component @P (x: f32)
ecs.component @Link (to: !ecs.entity)
ecs.archetype @Node (@P, @Link) capacity 10

// CHECK-NOT: !ecs.entity
// CHECK-NOT: unrealized_conversion_cast

// An entity parameter becomes an integer parameter.
// CHECK-LABEL: func.func private @point(
// CHECK-SAME: %[[TARGET:[^:]*]]: i64, %{{[^:]*}}: memref<{{.*}}xi8>)
ecs.system @point(%target: !ecs.entity) reads [@P] writes [@Link] {
  ecs.query (%l: !ecs.ref<@Link, mut>, %p: !ecs.ref<@P>) {
    // An id loaded from the id column, chosen against a parameter and
    // stored into an entity field.
    // CHECK:      scf.for %[[I:.*]] =
    // CHECK:        %[[SELF:.*]] = memref.load %{{.*}}[%[[I]]] : memref<10xi64>
    // CHECK:        %[[PICK:.*]] = scf.if %{{.*}} -> (i64) {
    // CHECK-NEXT:     scf.yield %[[SELF]] : i64
    // CHECK-NEXT:   } else {
    // CHECK-NEXT:     scf.yield %[[TARGET]] : i64
    // CHECK-NEXT:   }
    // CHECK:        %[[CHOSEN:.*]] = arith.select %{{.*}}, %[[PICK]], %[[TARGET]] : i64
    // CHECK-NEXT:   memref.store %[[CHOSEN]], %{{.*}}[%[[I]]] : memref<10xi64>
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
// CHECK-SAME: %[[T:[^:]*]]: i64,
// CHECK: call @point(%[[T]], %{{.*}}) : (i64, memref<{{.*}}xi8>) -> ()
ecs.schedule @frame(%t: !ecs.entity) {
  ecs.run @point(%t) : !ecs.entity
}
