// RUN: ecs-opt %s --ecs-lower-to-loops=fuse-systems=1 --symbol-dce | FileCheck %s

ecs.component @P (x: f32)
ecs.component @V (dx: f32)
ecs.archetype @A (@P, @V)
ecs.archetype @B (@P)

func.func private @log()

// Fused systems are no longer called, so symbol-dce removes them; the
// opaque one is still called and stays. System functions are emitted where
// the systems were declared, ahead of the schedules.
// CHECK-NOT: func.func private @accelerate
// CHECK-NOT: func.func private @move
// CHECK-NOT: func.func private @shift
// CHECK: func.func private @logs

ecs.system @accelerate(%k: f32) writes [@V] {
  // Ops outside queries are cloned once per run, with parameters mapped to
  // the run's arguments.
  %twice = arith.addf %k, %k : f32
  ecs.query (%v: !ecs.ref<@V, mut>) {
    %dx = ecs.get %v "dx" : !ecs.ref<@V, mut> -> f32
    %n = arith.addf %dx, %twice : f32
    ecs.set %v "dx", %n : !ecs.ref<@V, mut>, f32
  }
}
// Reads what accelerate writes: they conflict, and still fuse.
ecs.system @move() reads [@V] writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>, %v: !ecs.ref<@V>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %dx = ecs.get %v "dx" : !ecs.ref<@V> -> f32
    %n = arith.addf %x, %dx : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @shift(%d: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @logs() {
  func.call @log() : () -> ()
}

// One loop per archetype; each holds, in program order, the bodies of the
// queries that match it. Stages are dissolved first.
// CHECK-LABEL: func.func @frame(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[D:[^:]*]]: f32,
// CHECK-SAME: %[[NA:[^:]*]]: index, %[[AX:[^:]*]]: memref<?xf32>, %[[ADX:[^:]*]]: memref<?xf32>,
// CHECK-SAME: %[[NB:[^:]*]]: index, %[[BX:[^:]*]]: memref<?xf32>)
// CHECK:      %[[TWICE:.*]] = arith.addf %[[K]], %[[K]]
// CHECK:      scf.for %[[I:.*]] = %{{.*}} to %[[NA]]
// CHECK-NEXT:   %[[DX:.*]] = memref.load %[[ADX]][%[[I]]]
// CHECK-NEXT:   %[[DX2:.*]] = arith.addf %[[DX]], %[[TWICE]]
// CHECK-NEXT:   memref.store %[[DX2]], %[[ADX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[AX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[ADX]][%[[I]]]
// CHECK-NEXT:   arith.addf
// CHECK-NEXT:   memref.store %{{.*}}, %[[AX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[AX]][%[[I]]]
// CHECK-NEXT:   arith.addf %{{.*}}, %[[D]]
// CHECK-NEXT:   memref.store %{{.*}}, %[[AX]][%[[I]]]
// CHECK-NEXT: }
// CHECK:      scf.for %[[J:.*]] = %{{.*}} to %[[NB]]
// CHECK-NEXT:   memref.load %[[BX]][%[[J]]]
// CHECK-NEXT:   arith.addf %{{.*}}, %[[D]]
// CHECK-NEXT:   memref.store %{{.*}}, %[[BX]][%[[J]]]
// CHECK-NEXT: }
// CHECK-NEXT: return
ecs.schedule @frame(%k: f32, %d: f32) {
  ecs.stage {
    ecs.run @accelerate(%k) : f32
  }
  ecs.stage {
    ecs.run @move()
    ecs.run @shift(%d) : f32
  }
}

// An opaque system and an op with effects both end a fused sequence. @shift
// matches both archetypes, so each sequence is two loops (A, then B).
// CHECK-LABEL: func.func @barriers(
// CHECK-SAME: %{{[^:]*}}: f32, %[[NA:[^:]*]]: index, %{{[^:]*}}: memref<?xf32>, %{{[^:]*}}: memref<?xf32>, %[[NB:[^:]*]]: index
// CHECK:      scf.for {{.*}} to %[[NA]]
// CHECK:      scf.for {{.*}} to %[[NB]]
// CHECK:      }
// CHECK-NEXT: call @logs(
// CHECK:      scf.for {{.*}} to %[[NA]]
// CHECK:      scf.for {{.*}} to %[[NB]]
// CHECK:      }
// CHECK-NEXT: call @log()
// CHECK:      scf.for {{.*}} to %[[NA]]
// CHECK:      scf.for {{.*}} to %[[NB]]
// CHECK:      }
// CHECK-NEXT: return
ecs.schedule @barriers(%d: f32) {
  ecs.run @shift(%d) : f32
  ecs.run @logs()
  ecs.run @shift(%d) : f32
  func.call @log() : () -> ()
  ecs.run @shift(%d) : f32
}
