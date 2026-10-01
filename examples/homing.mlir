// Relations: a missile refers to its target ship by id (a Target component
// holding an !ent.entity) and steers towards the ship's position, read with
// ent.lookup. A ship that leaves the arena is despawned; a missile whose
// target is gone keeps its course, because the lookup reports the target
// as not found.

ent.component @Position (x: f32)
ent.component @Velocity (dx: f32)
ent.component @Hull (hp: f32)
ent.component @Target (entity: !ent.entity)

ent.archetype @Ship (@Position, @Velocity, @Hull) capacity 16
ent.archetype @Missile (@Position, @Velocity, @Target) capacity 64

// Writes Velocity and looks up Position: a query may look up what it does
// not change.
ent.system @steer(%gain: f32) reads [@Position, @Target] writes [@Velocity] {
  ent.query (%p: !ent.ref<@Position>, %v: !ent.ref<@Velocity, mut>,
             %t: !ent.ref<@Target>) {
    %target = ent.get %t "entity" : !ent.ref<@Target> -> !ent.entity
    %tx, %found = ent.lookup %target @Position "x" : f32
    scf.if %found {
      %x = ent.get %p "x" : !ent.ref<@Position> -> f32
      %gap = arith.subf %tx, %x : f32
      %dx = arith.mulf %gap, %gain : f32
      ent.set %v "dx", %dx : !ent.ref<@Velocity, mut>, f32
    }
  }
}

ent.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
  ent.query (%p: !ent.ref<@Position, mut>, %v: !ent.ref<@Velocity>) {
    %x = ent.get %p "x" : !ent.ref<@Position, mut> -> f32
    %dx = ent.get %v "dx" : !ent.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ent.set %p "x", %nx : !ent.ref<@Position, mut>, f32
  }
}

// Ships (they have a Hull) past the edge leave the arena.
ent.system @leave(%edge: f32) reads [@Position, @Hull] writes [@Ship] {
  ent.query (%p: !ent.ref<@Position>, %h: !ent.ref<@Hull>) {
    %x = ent.get %p "x" : !ent.ref<@Position> -> f32
    %out = arith.cmpf ogt, %x, %edge : f32
    scf.if %out {
      ent.despawn
    }
  }
}

ent.schedule @frame(%dt: f32) {
  %gain = arith.constant 1.0 : f32
  %edge = arith.constant 20.0 : f32
  ent.run @steer(%gain) : f32
  ent.run @move(%dt) : f32
  ent.run @leave(%edge) : f32
}
