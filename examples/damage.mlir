// Cross-entity writes: a torpedo refers to its target ship by id and, when
// its fuse runs out, deals its damage to the ship's hull with ent.apply and
// is despawned. Applies are combined when the query ends, so several hits
// on one ship in a frame add up, whatever order the torpedoes run in. A
// ship whose hull is gone sinks; a torpedo aimed at a sunk ship hits
// nothing, even if a new ship took over its slot.

ent.component @Hull (hp: f32)
ent.component @Fuse (seconds: f32)
ent.component @Warhead (damage: f32)
ent.component @Target (entity: !ent.entity)

ent.archetype @Ship (@Hull) capacity 16
ent.archetype @Torpedo (@Fuse, @Warhead, @Target) capacity 64

// Writes Hull of other entities (the targets) and despawns torpedoes.
ent.system @detonate(%dt: f32) reads [@Warhead, @Target]
    writes [@Fuse, @Hull, @Torpedo] {
  ent.query (%f: !ent.ref<@Fuse, mut>, %w: !ent.ref<@Warhead>,
             %t: !ent.ref<@Target>) {
    %s = ent.get %f "seconds" : !ent.ref<@Fuse, mut> -> f32
    %left = arith.subf %s, %dt : f32
    ent.set %f "seconds", %left : !ent.ref<@Fuse, mut>, f32
    %zero = arith.constant 0.0 : f32
    %done = arith.cmpf ole, %left, %zero : f32
    scf.if %done {
      %target = ent.get %t "entity" : !ent.ref<@Target> -> !ent.entity
      %damage = ent.get %w "damage" : !ent.ref<@Warhead> -> f32
      %loss = arith.negf %damage : f32
      ent.apply %target @Hull "hp" add %loss : f32
      ent.despawn
    }
  }
}

ent.system @sink() reads [@Hull] writes [@Ship] {
  ent.query (%h: !ent.ref<@Hull>) {
    %hp = ent.get %h "hp" : !ent.ref<@Hull> -> f32
    %zero = arith.constant 0.0 : f32
    %gone = arith.cmpf ole, %hp, %zero : f32
    scf.if %gone {
      ent.despawn
    }
  }
}

ent.schedule @frame(%dt: f32) {
  ent.run @detonate(%dt) : f32
  ent.run @sink()
}
