// RUN: ecs-opt %S/../../examples/integrate.mlir | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ecs-opt %S/../../examples/integrate.mlir | ecs-opt | FileCheck %s

// CHECK: ecs.component @Position (x: f32, y: f32)
// CHECK: ecs.component @Mass (kg: f32)
// CHECK: ecs.archetype @Body (@Position, @Velocity, @Mass)
// CHECK: ecs.archetype @Scenery (@Position)
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
