// RUN: ecs-opt %S/../../examples/integrate.mlir | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ecs-opt %S/../../examples/integrate.mlir | ecs-opt | FileCheck %s

// CHECK: ecs.component @Position (x: f32, y: f32)
// CHECK: ecs.component @Mass (kg: f32)
// CHECK: ecs.archetype @Body (@Position, @Velocity, @Mass) capacity 10000000
// CHECK: ecs.archetype @Scenery (@Position) capacity 10000000
// CHECK-LABEL: ecs.system @gravity(%{{.*}}: f32, %{{.*}}: f32) reads [@Mass] writes [@Velocity] {
// CHECK:   ecs.query (%[[V:.*]]: !ecs.ref<@Velocity, mut>, %{{.*}}: !ecs.ref<@Mass>) {
// CHECK:     ecs.get %[[V]] "dy" : <@Velocity, mut> -> f32
// CHECK:     ecs.set %[[V]] "dy", %{{.*}} : <@Velocity, mut>, f32
// CHECK-NEXT: }
// CHECK-NEXT: }
// CHECK: ecs.schedule @frame(%[[DT:.*]]: f32) {
// CHECK:   ecs.run @gravity(%[[DT]], %{{.*}}) : f32, f32
// CHECK:   ecs.run @integrate(%[[DT]]) : f32
// CHECK-NOT: ecs.yield

// A system without access lists and a component with quoted field names.
// RUN: ecs-opt %s | FileCheck %s --check-prefix=MISC
// MISC: ecs.component @Tag ()
// MISC: ecs.component @Named ("with space": i32, idx: index)
// MISC: ecs.system @noop() {
// MISC: ecs.schedule @staged() {
// MISC-NEXT: ecs.stage {
// MISC-NEXT:   ecs.run @noop()
// MISC-NEXT: }
// MISC-NEXT: ecs.stage {
// MISC-NEXT: }
// MISC: ecs.resource @Clock (dt: f32, frame: i64)
// MISC-LABEL: ecs.system @tick() reads [@Tag] writes [@Clock] {
// MISC-NEXT:   %[[F:.*]] = ecs.read @Clock "frame" : i64
// MISC:        ecs.write @Clock "frame", %{{.*}} : i64
// MISC-NEXT:   ecs.query (%{{.*}}: !ecs.ref<@Tag>) {
// MISC-NEXT:     ecs.read @Clock "dt" : f32
// MISC: ecs.component @Stunned (seconds: f32)
// MISC: ecs.archetype @Character (@Tag, optional @Stunned) capacity 10
// MISC-LABEL: ecs.system @stun(%{{.*}}: f32) reads [@Tag] writes [@Stunned] {
// MISC:          ecs.add @Stunned(%{{.*}}) : f32
// MISC:          ecs.remove @Stunned
// MISC-LABEL: ecs.system @fire(%{{.*}}: f32) reads [@Tag] writes [@Character] {
// MISC:          ecs.spawn @Character()
// MISC:          ecs.despawn
// MISC-LABEL: ecs.system @follow() reads [@Tag, @Named] {
// MISC:          %{{.*}}, %{{.*}} = ecs.lookup %{{.*}} @Named "idx" : index
// MISC-LABEL: ecs.system @hit(%{{.*}}: i32) reads [@Tag] writes [@Named] {
// MISC:          ecs.apply %{{.*}} @Named "with space" add %{{.*}} : i32
// MISC-NEXT:     ecs.apply %{{.*}} @Named "idx" max %{{.*}} : index
// MISC-LABEL: ecs.system @react() reads [@Named, @Stunned] writes [@Tag] {
// MISC-NEXT:   ecs.query (%{{.*}}: !ecs.ref<@Tag, mut>) on [changed @Named "idx" log 4096, changed @Named, added @Stunned log 0, removed @Stunned] {
// Without declarations a system's access is inferred and none is printed;
// an empty declaration is a contract and stays.
// MISC-LABEL: ecs.system @inferred() {
// MISC:       ecs.system @nothing() reads [] {
// MISC-LABEL: ecs.system @score() reads [@Tag] writes [@Clock] {
// MISC:          ecs.accumulate @Clock "frame" add %{{.*}} : i64
// MISC-NEXT:     ecs.accumulate @Clock "dt" max %{{.*}} : f32
ecs.component @Tag ()
ecs.component @Named ("with space": i32, idx: index)
ecs.system @noop() {
}
ecs.schedule @staged() {
  ecs.stage {
    ecs.run @noop()
  }
  ecs.stage {
  }
}
ecs.resource @Clock (dt: f32, frame: i64)
ecs.system @tick() reads [@Tag] writes [@Clock] {
  %frame = ecs.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %frame, %one : i64
  ecs.write @Clock "frame", %next : i64
  ecs.query (%t: !ecs.ref<@Tag>) {
    %dt = ecs.read @Clock "dt" : f32
  }
}
ecs.component @Stunned (seconds: f32)
ecs.archetype @Character (@Tag, optional @Stunned) capacity 10
ecs.system @stun(%s: f32) reads [@Tag] writes [@Stunned] {
  ecs.query (%t: !ecs.ref<@Tag>) {
    ecs.add @Stunned(%s) : f32
  }
  ecs.query (%st: !ecs.ref<@Stunned, mut>) {
    ecs.remove @Stunned
  }
}
ecs.system @fire(%s: f32) reads [@Tag] writes [@Character] {
  ecs.spawn @Character()
  ecs.query (%t: !ecs.ref<@Tag>) {
    ecs.despawn
  }
}
ecs.system @follow() reads [@Tag, @Named] {
  ecs.query (%t: !ecs.ref<@Tag>) {
    %id = ecs.entity
    %idx, %found = ecs.lookup %id @Named "idx" : index
  }
}
ecs.system @hit(%v: i32) reads [@Tag] writes [@Named] {
  ecs.query (%t: !ecs.ref<@Tag>) {
    %id = ecs.entity
    %i = arith.constant 3 : index
    ecs.apply %id @Named "with space" add %v : i32
    ecs.apply %id @Named "idx" max %i : index
  }
}
ecs.system @react() reads [@Named, @Stunned] writes [@Tag] {
  ecs.query (%t: !ecs.ref<@Tag, mut>)
      on [changed @Named "idx" log 4096, changed @Named, added @Stunned log 0,
          removed @Stunned] {
  }
}
ecs.system @inferred() {
  ecs.query (%t: !ecs.ref<@Tag>) {
    %dt = ecs.read @Clock "dt" : f32
  }
}
ecs.system @nothing() reads [] {
}
ecs.system @score() reads [@Tag] writes [@Clock] {
  ecs.query (%t: !ecs.ref<@Tag>) {
    %one = arith.constant 1 : i64
    %dt = arith.constant 0.5 : f32
    ecs.accumulate @Clock "frame" add %one : i64
    ecs.accumulate @Clock "dt" max %dt : f32
  }
}
