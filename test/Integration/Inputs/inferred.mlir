// An inferred archetype end to end: units are spawned with a Hull and a
// Shield, and a system strips the Shield, so the compiler makes Shield
// optional in @Hull_Shield; a spawned unit must still start with it.

ecs.component @Hull (hp: f32) capacity 4
ecs.component @Shield ()

ecs.system @make(%spawn: i1) {
  scf.if %spawn {
    %hp = arith.constant 10.0 : f32
    %id = ecs.spawn (@Hull, @Shield)(%hp) : f32
  }
}

// Units with a shield gain 1 hp, once: then the shield goes.
ecs.system @charge() {
  ecs.query (%h: !ecs.ref<@Hull, mut>, %s: !ecs.ref<@Shield>) {
    %hp = ecs.get %h "hp" : !ecs.ref<@Hull, mut> -> f32
    %one = arith.constant 1.0 : f32
    %more = arith.addf %hp, %one : f32
    ecs.set %h "hp", %more : !ecs.ref<@Hull, mut>, f32
    ecs.remove @Shield
  }
}

ecs.schedule @frame(%spawn: i1) {
  ecs.run @make(%spawn) : i1
  ecs.run @charge()
}
