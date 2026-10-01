// Cross-entity writes for bench/apply: every gun deals its damage to one
// target ship with ent.apply. Nothing is despawned or moved, so ids are
// Rows ids (fire_generational.mlir forces generational ones).

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
