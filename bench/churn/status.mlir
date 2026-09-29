// The churn workload of churn.c as an ECS program: Status is an optional
// component of Character. The host applies the precomputed gains and
// losses through the generated header; the program runs move and status.

ecs.component @Position (x: f32, y: f32)
ecs.component @Velocity (dx: f32, dy: f32)
ecs.component @Status (remaining: f32)

ecs.archetype @Character (@Position, @Velocity, optional @Status)
    capacity 1000000

ecs.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
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

// Runs only for entities that currently have Status.
ecs.system @status(%dt: f32) writes [@Velocity, @Status] {
  ecs.query (%v: !ecs.ref<@Velocity, mut>, %s: !ecs.ref<@Status, mut>) {
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity, mut> -> f32
    %slow = arith.constant 0.99 : f32
    %ndx = arith.mulf %dx, %slow : f32
    ecs.set %v "dx", %ndx : !ecs.ref<@Velocity, mut>, f32
    %t = ecs.get %s "remaining" : !ecs.ref<@Status, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ecs.set %s "remaining", %left : !ecs.ref<@Status, mut>, f32
  }
}

ecs.schedule @frame(%dt: f32) {
  ecs.run @move(%dt) : f32
  ecs.run @status(%dt) : f32
}
