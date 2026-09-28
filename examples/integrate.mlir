// The toy simulation that the milestones grow around: bodies move by their
// velocity, gravity pulls every body with mass down, and a schedule runs both
// systems once per frame.

ecs.component @Position (x: f32, y: f32)
ecs.component @Velocity (dx: f32, dy: f32)
ecs.component @Mass (kg: f32)

ecs.system @gravity(%dt: f32, %g: f32) reads [@Mass] writes [@Velocity] {
  // Mass is bound so the query only visits bodies that have one.
  ecs.query (%v: !ecs.ref<@Velocity, mut>, %m: !ecs.ref<@Mass>) {
    %dy = ecs.get %v "dy" : !ecs.ref<@Velocity, mut> -> f32
    %dv = arith.mulf %g, %dt : f32
    %ndy = arith.subf %dy, %dv : f32
    ecs.set %v "dy", %ndy : !ecs.ref<@Velocity, mut>, f32
  }
}

ecs.system @integrate(%dt: f32) reads [@Velocity] writes [@Position] {
  ecs.query (%p: !ecs.ref<@Position, mut>, %v: !ecs.ref<@Velocity>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position, mut> -> f32
    %y = ecs.get %p "y" : !ecs.ref<@Position, mut> -> f32
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity> -> f32
    %dy = ecs.get %v "dy" : !ecs.ref<@Velocity> -> f32
    %sx = arith.mulf %dx, %dt : f32
    %sy = arith.mulf %dy, %dt : f32
    %nx = arith.addf %x, %sx : f32
    %ny = arith.addf %y, %sy : f32
    ecs.set %p "x", %nx : !ecs.ref<@Position, mut>, f32
    ecs.set %p "y", %ny : !ecs.ref<@Position, mut>, f32
  }
}

ecs.schedule @frame(%dt: f32) {
  %g = arith.constant 9.81 : f32
  ecs.run @gravity(%dt, %g) : f32, f32
  ecs.run @integrate(%dt) : f32
}
