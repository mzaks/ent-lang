// The same program as status.mlir, stored differently: Stunned is a
// regular component, so a stunned character lives in @StunnedCharacter and
// gaining or losing the stun moves it between the two archetypes. The
// systems below are identical to status.mlir's (the integration test checks
// that); ent.add and ent.remove move entities here and set a presence byte
// there, because the archetypes say so.

ent.component @Position (x: f32)
ent.component @Velocity (dx: f32)
ent.component @Stunned (seconds: f32)

ent.archetype @Character (@Position, @Velocity) capacity 1000
ent.archetype @StunnedCharacter (@Position, @Velocity, @Stunned)
    capacity 1000

ent.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
  ent.query (%p: !ent.ref<@Position, mut>, %v: !ent.ref<@Velocity>) {
    %x = ent.get %p "x" : !ent.ref<@Position, mut> -> f32
    %dx = ent.get %v "dx" : !ent.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ent.set %p "x", %nx : !ent.ref<@Position, mut>, f32
  }
}

ent.system @trap(%wall: f32) reads [@Position] writes [@Velocity, @Stunned] {
  ent.query (%p: !ent.ref<@Position>, %v: !ent.ref<@Velocity, mut>) {
    %x = ent.get %p "x" : !ent.ref<@Position> -> f32
    %hit = arith.cmpf oge, %x, %wall : f32
    scf.if %hit {
      %dx = ent.get %v "dx" : !ent.ref<@Velocity, mut> -> f32
      %back = arith.negf %dx : f32
      ent.set %v "dx", %back : !ent.ref<@Velocity, mut>, f32
      %second = arith.constant 1.0 : f32
      ent.add @Stunned(%second) : f32
    }
  }
}

// Runs only for stunned characters.
ent.system @recover(%dt: f32) writes [@Stunned] {
  ent.query (%s: !ent.ref<@Stunned, mut>) {
    %t = ent.get %s "seconds" : !ent.ref<@Stunned, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ent.set %s "seconds", %left : !ent.ref<@Stunned, mut>, f32
    %zero = arith.constant 0.0 : f32
    %over = arith.cmpf ole, %left, %zero : f32
    scf.if %over {
      ent.remove @Stunned
    }
  }
}

ent.schedule @frame(%dt: f32) {
  %wall = arith.constant 10.0 : f32
  ent.run @move(%dt) : f32
  ent.run @trap(%wall) : f32
  ent.run @recover(%dt) : f32
}
