// The toy simulation that the milestones grow around: bodies move by their
// velocity, gravity pulls every body with mass down, and a schedule runs both
// systems once per frame. Particles move but have no mass, so gravity skips
// them; instead wind pushes them sideways and their lifetime runs out.
// Scenery has a position only, so no system touches it. There is exactly
// one player, which only moves. Two resources hold world state that is not
// an entity: the wind's strength, which the wind query reads, and a frame
// counter that `tick` advances at the end of each frame.
//
// With --ent-schedule, gravity, wind and decay share the first stage:
// gravity and wind both declare `writes [@Velocity]`, but gravity only
// writes Body.Velocity.dy and wind only Particle.Velocity.dx. Integrate
// reads both, so it runs in the second stage. `tick` touches only
// Clock.frame and joins the first stage.
//
// With fuse-systems, gravity, wind, integrate and decay fuse into one loop
// per archetype; `tick` writes a resource, so it stays a separate call.

ent.component @Position (x: f32, y: f32)
ent.component @Velocity (dx: f32, dy: f32)
ent.component @Mass (kg: f32)
ent.component @Lifetime (seconds: f32)

ent.resource @Wind (strength: f32)
ent.resource @Clock (frame: i64)

ent.archetype @Body (@Position, @Velocity, @Mass) capacity 10000000
ent.archetype @Particle (@Position, @Velocity, @Lifetime) capacity 10000000
ent.archetype @Scenery (@Position) capacity 10000000
ent.archetype @Player (@Position, @Velocity) capacity 1

ent.system @gravity(%dt: f32, %g: f32) reads [@Mass] writes [@Velocity] {
  // Mass is bound so the query only visits bodies that have one.
  ent.query (%v: !ent.ref<@Velocity, mut>, %m: !ent.ref<@Mass>) {
    %dy = ent.get %v "dy" : !ent.ref<@Velocity, mut> -> f32
    %dv = arith.mulf %g, %dt : f32
    %ndy = arith.subf %dy, %dv : f32
    ent.set %v "dy", %ndy : !ent.ref<@Velocity, mut>, f32
  }
}

ent.system @wind(%dt: f32) reads [@Lifetime, @Wind] writes [@Velocity] {
  // Lifetime is bound only to select particles; it is never read.
  ent.query (%v: !ent.ref<@Velocity, mut>, %l: !ent.ref<@Lifetime>) {
    %dx = ent.get %v "dx" : !ent.ref<@Velocity, mut> -> f32
    // The same for every particle; loaded once before the loop.
    %w = ent.read @Wind "strength" : f32
    %dv = arith.mulf %w, %dt : f32
    %ndx = arith.addf %dx, %dv : f32
    ent.set %v "dx", %ndx : !ent.ref<@Velocity, mut>, f32
  }
}

ent.system @decay(%dt: f32) writes [@Lifetime] {
  ent.query (%l: !ent.ref<@Lifetime, mut>) {
    %t = ent.get %l "seconds" : !ent.ref<@Lifetime, mut> -> f32
    %nt = arith.subf %t, %dt : f32
    ent.set %l "seconds", %nt : !ent.ref<@Lifetime, mut>, f32
  }
}

ent.system @integrate(%dt: f32) reads [@Velocity] writes [@Position] {
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

ent.system @tick() writes [@Clock] {
  %frame = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %frame, %one : i64
  ent.write @Clock "frame", %next : i64
}

ent.schedule @frame(%dt: f32) {
  %g = arith.constant 9.81 : f32
  ent.run @gravity(%dt, %g) : f32, f32
  ent.run @wind(%dt) : f32
  ent.run @integrate(%dt) : f32
  ent.run @decay(%dt) : f32
  ent.run @tick()
}
