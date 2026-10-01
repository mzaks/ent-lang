// fire.mlir with a despawn that is never scheduled: the program can
// despawn, so the compiler chooses generational ids, and every apply
// checks a target's slot and generation in the entity table.

ent.component @Hull (hp: f32)
ent.component @Gun (damage: f32, target: !ent.entity)

ent.archetype @Ship (@Hull) capacity 1000000
ent.archetype @Turret (@Gun) capacity 1000000

ent.system @fire() reads [@Gun] writes [@Hull] {
  ent.query (%g: !ent.ref<@Gun>) {
    %target = ent.get %g "target" : !ent.ref<@Gun> -> !ent.entity
    %damage = ent.get %g "damage" : !ent.ref<@Gun> -> f32
    %loss = arith.negf %damage : f32
    ent.apply %target @Hull "hp" add %loss : f32
  }
}

ent.schedule @frame() {
  ent.run @fire()
}

// Not scheduled: only here so that ids are generational.
ent.system @scrap() reads [@Hull] writes [@Ship] {
  ent.query (%h: !ent.ref<@Hull>) {
    ent.despawn
  }
}
