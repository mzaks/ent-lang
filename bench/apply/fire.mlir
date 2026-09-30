// Cross-entity writes for bench/apply: every gun deals its damage to one
// target ship with ecs.apply. Nothing is despawned or moved, so ids are
// Rows ids (fire_generational.mlir forces generational ones).

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
