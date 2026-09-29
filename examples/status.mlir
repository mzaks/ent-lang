// Optional components: characters move; one that reaches the wall bounces
// back and is stunned for a second; recover counts the stun down and
// removes it when it runs out. Stunned is optional in the Character
// archetype, so gaining and losing it writes the character's own row
// instead of moving it to another archetype.

ecs.component @Position (x: f32)
ecs.component @Velocity (dx: f32)
ecs.component @Stunned (seconds: f32)

ecs.archetype @Character (@Position, @Velocity, optional @Stunned)
    capacity 1000

ecs.system @move(%dt: f32) reads [@Velocity] writes [@Position] {
  ecs.query (%p: !ecs.ref<@Position, mut>, %v: !ecs.ref<@Velocity>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position, mut> -> f32
    %dx = ecs.get %v "dx" : !ecs.ref<@Velocity> -> f32
    %step = arith.mulf %dx, %dt : f32
    %nx = arith.addf %x, %step : f32
    ecs.set %p "x", %nx : !ecs.ref<@Position, mut>, f32
  }
}

ecs.system @trap(%wall: f32) reads [@Position] writes [@Velocity, @Stunned] {
  ecs.query (%p: !ecs.ref<@Position>, %v: !ecs.ref<@Velocity, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@Position> -> f32
    %hit = arith.cmpf oge, %x, %wall : f32
    scf.if %hit {
      %dx = ecs.get %v "dx" : !ecs.ref<@Velocity, mut> -> f32
      %back = arith.negf %dx : f32
      ecs.set %v "dx", %back : !ecs.ref<@Velocity, mut>, f32
      %second = arith.constant 1.0 : f32
      ecs.add @Stunned(%second) : f32
    }
  }
}

// Runs only for stunned characters.
ecs.system @recover(%dt: f32) writes [@Stunned] {
  ecs.query (%s: !ecs.ref<@Stunned, mut>) {
    %t = ecs.get %s "seconds" : !ecs.ref<@Stunned, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ecs.set %s "seconds", %left : !ecs.ref<@Stunned, mut>, f32
    %zero = arith.constant 0.0 : f32
    %over = arith.cmpf ole, %left, %zero : f32
    scf.if %over {
      ecs.remove @Stunned
    }
  }
}

ecs.schedule @frame(%dt: f32) {
  %wall = arith.constant 10.0 : f32
  ecs.run @move(%dt) : f32
  ecs.run @trap(%wall) : f32
  ecs.run @recover(%dt) : f32
}
