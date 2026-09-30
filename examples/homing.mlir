// Relations: a missile refers to its target ship by id (a Target component
// holding an !ecs.entity) and steers towards the ship's position, read with
// ecs.lookup. A ship that leaves the arena is despawned; a missile whose
// target is gone keeps its course, because the lookup reports the target
// as not found.

ecs.component @Position (x: f32)
ecs.component @Velocity (dx: f32)
ecs.component @Hull (hp: f32)
ecs.component @Target (entity: !ecs.entity)

ecs.archetype @Ship (@Position, @Velocity, @Hull) capacity 16
ecs.archetype @Missile (@Position, @Velocity, @Target) capacity 64

// Writes Velocity and looks up Position: a query may look up what it does
// not change.
ecs.system @steer(%gain: f32) reads [@Position, @Target] writes [@Velocity] {
  ecs.query (%p: !ecs.ref<@Position>, %v: !ecs.ref<@Velocity, mut>,
             %t: !ecs.ref<@Target>) {
    %target = ecs.get %t "entity" : !ecs.ref<@Target> -> !ecs.entity
    %tx, %found = ecs.lookup %target @Position "x" : f32
    scf.if %found {
      %x = ecs.get %p "x" : !ecs.ref<@Position> -> f32
      %gap = arith.subf %tx, %x : f32
      %dx = arith.mulf %gap, %gain : f32
      ecs.set %v "dx", %dx : !ecs.ref<@Velocity, mut>, f32
    }
  }
}

ecs.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
  ecs.query (%p: !ecs.ref<@Position, mut>, %v: !ecs.ref<@Velocity>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position, mut> -> f32
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ecs.set %p "x", %nx : !ecs.ref<@Position, mut>, f32
  }
}

// Ships (they have a Hull) past the edge leave the arena.
ecs.system @leave(%edge: f32) reads [@Position, @Hull] writes [@Ship] {
  ecs.query (%p: !ecs.ref<@Position>, %h: !ecs.ref<@Hull>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position> -> f32
    %out = arith.cmpf ogt, %x, %edge : f32
    scf.if %out {
      ecs.despawn
    }
  }
}

ecs.schedule @frame(%dt: f32) {
  %gain = arith.constant 1.0 : f32
  %edge = arith.constant 20.0 : f32
  ecs.run @steer(%gain) : f32
  ecs.run @move(%dt) : f32
  ecs.run @leave(%edge) : f32
}
