// Structural changes: guns fire bullets on a cooldown (a spawn from inside
// a query over guns), bullets fly, and a bullet whose lifetime runs out is
// despawned (deferred to the end of the query, then swap-removed).

ecs.component @Position (x: f32)
ecs.component @Velocity (dx: f32)
ecs.component @Cooldown (seconds: f32, period: f32)
ecs.component @Lifetime (seconds: f32)

ecs.archetype @Gun (@Position, @Cooldown) capacity 4
ecs.archetype @Bullet (@Position, @Velocity, @Lifetime) capacity 64

ecs.system @shoot(%dt: f32) reads [@Position] writes [@Cooldown, @Bullet] {
  ecs.query (%p: !ecs.ref<@Position>, %c: !ecs.ref<@Cooldown, mut>) {
    %s = ecs.get %c "seconds" : !ecs.ref<@Cooldown, mut> -> f32
    %left = arith.subf %s, %dt : f32
    %zero = arith.constant 0.0 : f32
    %ready = arith.cmpf ole, %left, %zero : f32
    %next = scf.if %ready -> f32 {
      %x = ecs.get %p "x" : !ecs.ref<@Position> -> f32
      %speed = arith.constant 10.0 : f32
      %life = arith.constant 1.0 : f32
      ecs.spawn @Bullet(%x, %speed, %life) : f32, f32, f32
      %period = ecs.get %c "period" : !ecs.ref<@Cooldown, mut> -> f32
      %reload = arith.addf %left, %period : f32
      scf.yield %reload : f32
    } else {
      scf.yield %left : f32
    }
    ecs.set %c "seconds", %next : !ecs.ref<@Cooldown, mut>, f32
  }
}

ecs.system @fly(%dt: f32) reads [@Velocity] writes [@Position] {
  ecs.query (%p: !ecs.ref<@Position, mut>, %v: !ecs.ref<@Velocity>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position, mut> -> f32
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ecs.set %p "x", %nx : !ecs.ref<@Position, mut>, f32
  }
}

ecs.system @expire(%dt: f32) writes [@Lifetime, @Bullet] {
  ecs.query (%l: !ecs.ref<@Lifetime, mut>) {
    %t = ecs.get %l "seconds" : !ecs.ref<@Lifetime, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ecs.set %l "seconds", %left : !ecs.ref<@Lifetime, mut>, f32
    %zero = arith.constant 0.0 : f32
    %over = arith.cmpf ole, %left, %zero : f32
    scf.if %over {
      ecs.despawn
    }
  }
}

ecs.schedule @frame(%dt: f32) {
  ecs.run @shoot(%dt) : f32
  ecs.run @fly(%dt) : f32
  ecs.run @expire(%dt) : f32
}
