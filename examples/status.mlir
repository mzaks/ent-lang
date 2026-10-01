// Optional components: characters move; one that reaches the wall bounces
// back and is stunned for a second; recover counts the stun down and
// removes it when it runs out. Stunned is optional in the Character
// archetype, so gaining and losing it writes the character's own row
// instead of moving it to another archetype.

ent.component @Position (x: f32)
ent.component @Velocity (dx: f32)
ent.component @Stunned (seconds: f32)

ent.archetype @Character (@Position, @Velocity, optional @Stunned)
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
