// fire.mlir with a despawn that is never scheduled: the program can
// despawn, so the compiler chooses generational ids, and every apply
// checks a target's slot and generation in the entity table.

ecs.component @Hull (hp: f32)
ecs.component @Gun (damage: f32, target: !ecs.entity)

ecs.archetype @Ship (@Hull) capacity 1000000
ecs.archetype @Turret (@Gun) capacity 1000000

ecs.system @fire() reads [@Gun] writes [@Hull] {
  ecs.query (%g: !ecs.ref<@Gun>) {
    %target = ecs.get %g "target" : !ecs.ref<@Gun> -> !ecs.entity
    %damage = ecs.get %g "damage" : !ecs.ref<@Gun> -> f32
    %loss = arith.negf %damage : f32
    ecs.apply %target @Hull "hp" add %loss : f32
  }
}

ecs.schedule @frame() {
  ecs.run @fire()
}

// Not scheduled: only here so that ids are generational.
ecs.system @scrap() reads [@Hull] writes [@Ship] {
  ecs.query (%h: !ecs.ref<@Hull>) {
    ecs.despawn
  }
}
