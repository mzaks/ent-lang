// The toy simulation that the milestones grow around: bodies move by their
// velocity, gravity pulls every body with mass down, and a schedule runs both
// systems once per frame. Particles move but have no mass, so gravity skips
// them; instead wind pushes them sideways and their lifetime runs out.
// Scenery has a position only, so no system touches it. There is exactly
// one player, which only moves. Two resources hold world state that is not
// an entity: the wind's strength, which the wind query reads, and a frame
// counter that `tick` advances at the end of each frame.
//
// With --ecs-schedule, gravity, wind and decay share the first stage:
// gravity and wind both declare `writes [@Velocity]`, but gravity only
// writes Body.Velocity.dy and wind only Particle.Velocity.dx. Integrate
// reads both, so it runs in the second stage. `tick` touches only
// Clock.frame and joins the first stage.
//
// With fuse-systems, gravity, wind, integrate and decay fuse into one loop
// per archetype; `tick` writes a resource, so it stays a separate call.

ecs.component @Position (x: f32, y: f32)
ecs.component @Velocity (dx: f32, dy: f32)
ecs.component @Mass (kg: f32)
ecs.component @Lifetime (seconds: f32)

ecs.resource @Wind (strength: f32)
ecs.resource @Clock (frame: i64)

ecs.archetype @Body (@Position, @Velocity, @Mass) capacity 10000000
ecs.archetype @Particle (@Position, @Velocity, @Lifetime) capacity 10000000
ecs.archetype @Scenery (@Position) capacity 10000000
ecs.archetype @Player (@Position, @Velocity) capacity 1

ecs.system @gravity(%dt: f32, %g: f32) reads [@Mass] writes [@Velocity] {
  // Mass is bound so the query only visits bodies that have one.
  ecs.query (%v: !ecs.ref<@Velocity, mut>, %m: !ecs.ref<@Mass>) {
    %dy = ecs.get %v "dy" : !ecs.ref<@Velocity, mut> -> f32
    %dv = arith.mulf %g, %dt : f32
    %ndy = arith.subf %dy, %dv : f32
    ecs.set %v "dy", %ndy : !ecs.ref<@Velocity, mut>, f32
  }
}

ecs.system @wind(%dt: f32) reads [@Lifetime, @Wind] writes [@Velocity] {
  // Lifetime is bound only to select particles; it is never read.
  ecs.query (%v: !ecs.ref<@Velocity, mut>, %l: !ecs.ref<@Lifetime>) {
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity, mut> -> f32
    // The same for every particle; loaded once before the loop.
    %w = ecs.read @Wind "strength" : f32
    %dv = arith.mulf %w, %dt : f32
    %ndx = arith.addf %dx, %dv : f32
    ecs.set %v "dx", %ndx : !ecs.ref<@Velocity, mut>, f32
  }
}

ecs.system @decay(%dt: f32) writes [@Lifetime] {
  ecs.query (%l: !ecs.ref<@Lifetime, mut>) {
    %t = ecs.get %l "seconds" : !ecs.ref<@Lifetime, mut> -> f32
    %nt = arith.subf %t, %dt : f32
    ecs.set %l "seconds", %nt : !ecs.ref<@Lifetime, mut>, f32
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

ecs.system @tick() writes [@Clock] {
  %frame = ecs.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %frame, %one : i64
  ecs.write @Clock "frame", %next : i64
}

ecs.schedule @frame(%dt: f32) {
  %g = arith.constant 9.81 : f32
  ecs.run @gravity(%dt, %g) : f32, f32
  ecs.run @wind(%dt) : f32
  ecs.run @integrate(%dt) : f32
  ecs.run @decay(%dt) : f32
  ecs.run @tick()
}
