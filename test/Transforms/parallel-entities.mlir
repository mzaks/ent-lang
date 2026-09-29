// RUN: ecs-opt %s --ecs-lower-to-loops=parallel-entities=1 \
// RUN:   | FileCheck %s --check-prefix=DEFAULT
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=ALWAYS
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 fuse-systems=1" \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @S (x: f32)
ecs.component @L (x: f32)
ecs.archetype @Small (@S) capacity 1000
ecs.archetype @Large (@L) capacity 2000000

func.func private @log()

// The capacity is below the default threshold of 1e6: a parallel loop could
// never pay for its fork, so the loop is sequential without a check.
// DEFAULT-LABEL: func.func private @shiftSmall(
// DEFAULT-NOT:   scf.if
// DEFAULT-NOT:   scf.parallel
// DEFAULT:       scf.for
// ALWAYS-LABEL:  func.func private @shiftSmall(
// ALWAYS-NOT:    scf.if
// ALWAYS:        scf.parallel
ecs.system @shiftSmall(%d: f32) writes [@S] {
  ecs.query (%s: !ecs.ref<@S, mut>) {
    %x = ecs.get %s "x" : !ecs.ref<@S, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %s "x", %n : !ecs.ref<@S, mut>, f32
  }
}

// The capacity reaches the threshold: the count decides at run time.
// DEFAULT-LABEL: func.func private @shiftLarge(
// DEFAULT:       %[[N:.*]] = arith.index_cast
// DEFAULT:       %[[T:.*]] = arith.constant 1000000 : index
// DEFAULT:       %[[BIG:.*]] = arith.cmpi sge, %[[N]], %[[T]] : index
// DEFAULT:       scf.if %[[BIG]] {
// DEFAULT:         scf.parallel (%[[I:.*]]) = (%{{.*}}) to (%[[N]])
// DEFAULT:           memref.load %{{.*}}[%[[I]]]
// DEFAULT:           memref.store %{{.*}}, %{{.*}}[%[[I]]]
// DEFAULT:       } else {
// DEFAULT:         scf.for %[[J:.*]] = %{{.*}} to %[[N]]
// DEFAULT:           memref.load %{{.*}}[%[[J]]]
// DEFAULT:           memref.store %{{.*}}, %{{.*}}[%[[J]]]
// ALWAYS-LABEL:  func.func private @shiftLarge(
// ALWAYS-NOT:    scf.if
// ALWAYS:        scf.parallel
ecs.system @shiftLarge(%d: f32) writes [@L] {
  ecs.query (%l: !ecs.ref<@L, mut>) {
    %x = ecs.get %l "x" : !ecs.ref<@L, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %l "x", %n : !ecs.ref<@L, mut>, f32
  }
}

// A call in the body may have effects shared across entities: stay serial
// whatever the size.
// ALWAYS-LABEL: func.func private @shiftAndLog(
// ALWAYS-NOT:   scf.parallel
// ALWAYS:       scf.for
// ALWAYS:         call @log()
ecs.system @shiftAndLog() writes [@L] {
  ecs.query (%l: !ecs.ref<@L, mut>) {
    func.call @log() : () -> ()
  }
}

// Fused loops follow the same rule per archetype.
// FUSED-LABEL: func.func @frame(
// FUSED-NOT:   scf.parallel
// FUSED:       scf.for
// FUSED:       scf.if
// FUSED:         scf.parallel
// FUSED:       } else {
// FUSED:         scf.for
// FUSED:       call @shiftAndLog(
ecs.schedule @frame(%d: f32) {
  ecs.run @shiftSmall(%d) : f32
  ecs.run @shiftLarge(%d) : f32
  ecs.run @shiftAndLog()
}
