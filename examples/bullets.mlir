// Structural changes: guns fire bullets on a cooldown (a spawn from inside
// a query over guns), bullets fly, and a bullet whose lifetime runs out is
// despawned (deferred to the end of the query, then swap-removed).
//
// Bullets have no declared archetype: the spawn lists their components and
// the compiler infers one, @Position_Velocity_Lifetime, holding as many as
// the smallest capacity among those components (64). Guns are spawned only
// by the host, which the compiler cannot see, so their archetype is
// declared.

ent.component @Position (x: f32)
ent.component @Velocity (dx: f32) capacity 64
ent.component @Cooldown (seconds: f32, period: f32)
ent.component @Lifetime (seconds: f32) capacity 64

ent.archetype @Gun (@Position, @Cooldown) capacity 4

ent.system @shoot(%dt: f32) {
  ent.query (%p: !ent.ref<@Position>, %c: !ent.ref<@Cooldown, mut>) {
    %s = ent.get %c "seconds" : !ent.ref<@Cooldown, mut> -> f32
    %left = arith.subf %s, %dt : f32
    %zero = arith.constant 0.0 : f32
    %ready = arith.cmpf ole, %left, %zero : f32
    %next = scf.if %ready -> f32 {
      %x = ent.get %p "x" : !ent.ref<@Position> -> f32
      %speed = arith.constant 10.0 : f32
      %life = arith.constant 1.0 : f32
      ent.spawn (@Position, @Velocity, @Lifetime)(%x, %speed, %life)
          : f32, f32, f32
      %period = ent.get %c "period" : !ent.ref<@Cooldown, mut> -> f32
      %reload = arith.addf %left, %period : f32
      scf.yield %reload : f32
    } else {
      scf.yield %left : f32
    }
    ent.set %c "seconds", %next : !ent.ref<@Cooldown, mut>, f32
  }
}

ent.system @fly(%dt: f32) {
  ent.query (%p: !ent.ref<@Position, mut>, %v: !ent.ref<@Velocity>) {
    %x = ent.get %p "x" : !ent.ref<@Position, mut> -> f32
    %dx = ent.get %v "dx" : !ent.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ent.set %p "x", %nx : !ent.ref<@Position, mut>, f32
  }
}

ent.system @expire(%dt: f32) {
  ent.query (%l: !ent.ref<@Lifetime, mut>) {
    %t = ent.get %l "seconds" : !ent.ref<@Lifetime, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ent.set %l "seconds", %left : !ent.ref<@Lifetime, mut>, f32
    %zero = arith.constant 0.0 : f32
    %over = arith.cmpf ole, %left, %zero : f32
    scf.if %over {
      ent.despawn
    }
  }
}

ent.schedule @frame(%dt: f32) {
  ent.run @shoot(%dt) : f32
  ent.run @fly(%dt) : f32
  ent.run @expire(%dt) : f32
}
