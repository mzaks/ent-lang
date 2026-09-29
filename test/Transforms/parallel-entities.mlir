// RUN: ecs-opt %s --ecs-lower-to-loops=parallel-entities=1 | FileCheck %s
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 fuse-systems=1" \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 1000

func.func private @log()

// Entity-local body: iterations are independent.
// CHECK-LABEL: func.func private @shift(
// CHECK: scf.parallel (%[[I:.*]]) = (%{{.*}}) to (%{{.*}}) step (%{{.*}})
// CHECK:   memref.load %{{.*}}[%[[I]]]
// CHECK:   memref.store %{{.*}}, %{{.*}}[%[[I]]]
ecs.system @shift(%d: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}

// A call in the body may have effects shared across entities: stay serial.
// CHECK-LABEL: func.func private @shiftAndLog(
// CHECK-NOT: scf.parallel
// CHECK: scf.for
// CHECK:   call @log()
ecs.system @shiftAndLog() writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    func.call @log() : () -> ()
  }
}

// FUSED-LABEL: func.func @frame(
// FUSED:       scf.parallel
// FUSED-NOT:   scf.parallel
// FUSED:       call @shiftAndLog(
ecs.schedule @frame(%d: f32) {
  ecs.run @shift(%d) : f32
  ecs.run @shift(%d) : f32
  ecs.run @shiftAndLog()
}
