// RUN: ent-opt %S/../../examples/integrate.mlir | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %S/../../examples/integrate.mlir | ent-opt | FileCheck %s

// CHECK: ent.component @Position (x: f32, y: f32)
// CHECK: ent.component @Mass (kg: f32)
// CHECK: ent.archetype @Body (@Position, @Velocity, @Mass) capacity 10000000
// CHECK: ent.archetype @Scenery (@Position) capacity 10000000
// CHECK-LABEL: ent.system @gravity(%{{.*}}: f32, %{{.*}}: f32) reads [@Mass] writes [@Velocity] {
// CHECK:   ent.query (%[[V:.*]]: !ent.ref<@Velocity, mut>, %{{.*}}: !ent.ref<@Mass>) {
// CHECK:     ent.get %[[V]] "dy" : <@Velocity, mut> -> f32
// CHECK:     ent.set %[[V]] "dy", %{{.*}} : <@Velocity, mut>, f32
// CHECK-NEXT: }
// CHECK-NEXT: }
// CHECK: ent.schedule @frame(%[[DT:.*]]: f32) {
// CHECK:   ent.run @gravity(%[[DT]], %{{.*}}) : f32, f32
// CHECK:   ent.run @integrate(%[[DT]]) : f32
// CHECK-NOT: ent.yield

// A system without access lists and a component with quoted field names.
// RUN: ent-opt %s | FileCheck %s --check-prefix=MISC
// MISC: ent.component @Tag ()
// MISC: ent.component @Named ("with space": i32, idx: index)
// MISC: ent.system @noop() {
// MISC: ent.schedule @staged() {
// MISC-NEXT: ent.stage {
// MISC-NEXT:   ent.run @noop()
// MISC-NEXT: }
// MISC-NEXT: ent.stage {
// MISC-NEXT: }
// MISC: ent.resource @Clock (dt: f32, frame: i64)
// MISC-LABEL: ent.system @tick() reads [@Tag] writes [@Clock] {
// MISC-NEXT:   %[[F:.*]] = ent.read @Clock "frame" : i64
// MISC:        ent.write @Clock "frame", %{{.*}} : i64
// MISC-NEXT:   ent.query (%{{.*}}: !ent.ref<@Tag>) {
// MISC-NEXT:     ent.read @Clock "dt" : f32
// MISC: ent.component @Stunned (seconds: f32)
// MISC: ent.archetype @Character (@Tag, optional @Stunned) capacity 10
// MISC-LABEL: ent.system @stun(%{{.*}}: f32) reads [@Tag] writes [@Stunned] {
// MISC:          ent.add @Stunned(%{{.*}}) : f32
// MISC:          ent.remove @Stunned
// MISC-LABEL: ent.system @fire(%{{.*}}: f32) reads [@Tag] writes [@Character] {
// MISC:          ent.spawn @Character()
// MISC:          ent.despawn
// MISC-LABEL: ent.system @follow() reads [@Tag, @Named] {
// MISC:          %{{.*}}, %{{.*}} = ent.lookup %{{.*}} @Named "idx" : index
// MISC-LABEL: ent.system @hit(%{{.*}}: i32) reads [@Tag] writes [@Named] {
// MISC:          ent.apply %{{.*}} @Named "with space" add %{{.*}} : i32
// MISC-NEXT:     ent.apply %{{.*}} @Named "idx" max %{{.*}} : index
// MISC-LABEL: ent.system @react() reads [@Named, @Stunned] writes [@Tag] {
// MISC-NEXT:   ent.query (%{{.*}}: !ent.ref<@Tag, mut>) on [changed @Named "idx" log 4096, changed @Named, added @Stunned log 0, removed @Stunned] {
// Components may carry a capacity; spawns may list components, and after
// inference also name the archetype.
// MISC:       ent.component @Sized (s: i32) capacity 32
// MISC-LABEL: ent.system @spawnBoth(%{{.*}}: i32) {
// MISC-NEXT:    ent.spawn (@Sized)(%{{.*}}) : i32
// MISC-NEXT:    ent.spawn (@Sized) into @SizedOnly (%{{.*}}) : i32
// Without declarations a system's access is inferred and none is printed;
// an empty declaration is a contract and stays.
// MISC-LABEL: ent.system @inferred() {
// MISC:       ent.system @nothing() reads [] {
// MISC-LABEL: ent.system @score() reads [@Tag] writes [@Clock] {
// MISC:          ent.accumulate @Clock "frame" add %{{.*}} : i64
// MISC-NEXT:     ent.accumulate @Clock "dt" max %{{.*}} : f32
ent.component @Tag ()
ent.component @Named ("with space": i32, idx: index)
ent.system @noop() {
}
ent.schedule @staged() {
  ent.stage {
    ent.run @noop()
  }
  ent.stage {
  }
}
ent.resource @Clock (dt: f32, frame: i64)
ent.system @tick() reads [@Tag] writes [@Clock] {
  %frame = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %frame, %one : i64
  ent.write @Clock "frame", %next : i64
  ent.query (%t: !ent.ref<@Tag>) {
    %dt = ent.read @Clock "dt" : f32
  }
}
ent.component @Stunned (seconds: f32)
ent.archetype @Character (@Tag, optional @Stunned) capacity 10
ent.system @stun(%s: f32) reads [@Tag] writes [@Stunned] {
  ent.query (%t: !ent.ref<@Tag>) {
    ent.add @Stunned(%s) : f32
  }
  ent.query (%st: !ent.ref<@Stunned, mut>) {
    ent.remove @Stunned
  }
}
ent.system @fire(%s: f32) reads [@Tag] writes [@Character] {
  ent.spawn @Character()
  ent.query (%t: !ent.ref<@Tag>) {
    ent.despawn
  }
}
ent.system @follow() reads [@Tag, @Named] {
  ent.query (%t: !ent.ref<@Tag>) {
    %id = ent.entity
    %idx, %found = ent.lookup %id @Named "idx" : index
  }
}
ent.system @hit(%v: i32) reads [@Tag] writes [@Named] {
  ent.query (%t: !ent.ref<@Tag>) {
    %id = ent.entity
    %i = arith.constant 3 : index
    ent.apply %id @Named "with space" add %v : i32
    ent.apply %id @Named "idx" max %i : index
  }
}
ent.system @react() reads [@Named, @Stunned] writes [@Tag] {
  ent.query (%t: !ent.ref<@Tag, mut>)
      on [changed @Named "idx" log 4096, changed @Named, added @Stunned log 0,
          removed @Stunned] {
  }
}
ent.component @Sized (s: i32) capacity 32
ent.archetype @SizedOnly (@Sized) capacity 32
ent.system @spawnBoth(%v: i32) {
  %a = ent.spawn (@Sized)(%v) : i32
  %b = ent.spawn (@Sized) into @SizedOnly (%v) : i32
}
ent.system @inferred() {
  ent.query (%t: !ent.ref<@Tag>) {
    %dt = ent.read @Clock "dt" : f32
  }
}
ent.system @nothing() reads [] {
}
ent.system @score() reads [@Tag] writes [@Clock] {
  ent.query (%t: !ent.ref<@Tag>) {
    %one = arith.constant 1 : i64
    %dt = arith.constant 0.5 : f32
    ent.accumulate @Clock "frame" add %one : i64
    ent.accumulate @Clock "dt" max %dt : f32
  }
}
