// The churn workload of churn.c as an ECS program: Status is an optional
// component of Character. The host applies the precomputed gains and
// losses through the generated header; the program runs move and status.

ent.component @Position (x: f32, y: f32)
ent.component @Velocity (dx: f32, dy: f32)
ent.component @Status (remaining: f32)

ent.archetype @Character (@Position, @Velocity, optional @Status)
    capacity 1000000

ent.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
  ent.query (%p: !ent.ref<@Position, mut>, %v: !ent.ref<@Velocity>) {
    %x = ent.get %p "x" : !ent.ref<@Position, mut> -> f32
    %y = ent.get %p "y" : !ent.ref<@Position, mut> -> f32
    %dx = ent.get %v "dx" : !ent.ref<@Velocity> -> f32
    %dy = ent.get %v "dy" : !ent.ref<@Velocity> -> f32
    %sx = arith.mulf %dx, %dt : f32
    %sy = arith.mulf %dy, %dt : f32
    %nx = arith.addf %x, %sx : f32
    %ny = arith.addf %y, %sy : f32
    ent.set %p "x", %nx : !ent.ref<@Position, mut>, f32
    ent.set %p "y", %ny : !ent.ref<@Position, mut>, f32
  }
}

// Runs only for entities that currently have Status.
ent.system @status(%dt: f32) writes [@Velocity, @Status] {
  ent.query (%v: !ent.ref<@Velocity, mut>, %s: !ent.ref<@Status, mut>) {
    %dx = ent.get %v "dx" : !ent.ref<@Velocity, mut> -> f32
    %slow = arith.constant 0.99 : f32
    %ndx = arith.mulf %dx, %slow : f32
    ent.set %v "dx", %ndx : !ent.ref<@Velocity, mut>, f32
    %t = ent.get %s "remaining" : !ent.ref<@Status, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ent.set %s "remaining", %left : !ent.ref<@Status, mut>, f32
  }
}

ent.schedule @frame(%dt: f32) {
  ent.run @move(%dt) : f32
  ent.run @status(%dt) : f32
}
