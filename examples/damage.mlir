// Cross-entity writes: a torpedo refers to its target ship by id and, when
// its fuse runs out, deals its damage to the ship's hull with ecs.apply and
// is despawned. Applies are combined when the query ends, so several hits
// on one ship in a frame add up, whatever order the torpedoes run in. A
// ship whose hull is gone sinks; a torpedo aimed at a sunk ship hits
// nothing, even if a new ship took over its slot.

ecs.component @Hull (hp: f32)
ecs.component @Fuse (seconds: f32)
ecs.component @Warhead (damage: f32)
ecs.component @Target (entity: !ecs.entity)

ecs.archetype @Ship (@Hull) capacity 16
ecs.archetype @Torpedo (@Fuse, @Warhead, @Target) capacity 64

// Writes Hull of other entities (the targets) and despawns torpedoes.
ecs.system @detonate(%dt: f32) reads [@Warhead, @Target]
    writes [@Fuse, @Hull, @Torpedo] {
  ecs.query (%f: !ecs.ref<@Fuse, mut>, %w: !ecs.ref<@Warhead>,
             %t: !ecs.ref<@Target>) {
    %s = ecs.get %f "seconds" : !ecs.ref<@Fuse, mut> -> f32
    %left = arith.subf %s, %dt : f32
    ecs.set %f "seconds", %left : !ecs.ref<@Fuse, mut>, f32
    %zero = arith.constant 0.0 : f32
    %done = arith.cmpf ole, %left, %zero : f32
    scf.if %done {
      %target = ecs.get %t "entity" : !ecs.ref<@Target> -> !ecs.entity
      %damage = ecs.get %w "damage" : !ecs.ref<@Warhead> -> f32
      %loss = arith.negf %damage : f32
      ecs.apply %target @Hull "hp" add %loss : f32
      ecs.despawn
    }
  }
}

ecs.system @sink() reads [@Hull] writes [@Ship] {
  ecs.query (%h: !ecs.ref<@Hull>) {
    %hp = ecs.get %h "hp" : !ecs.ref<@Hull> -> f32
    %zero = arith.constant 0.0 : f32
    %gone = arith.cmpf ole, %hp, %zero : f32
    scf.if %gone {
      ecs.despawn
    }
  }
}

ecs.schedule @frame(%dt: f32) {
  ecs.run @detonate(%dt) : f32
  ecs.run @sink()
}
