// An inferred archetype end to end: units are spawned with a Hull and a
// Shield, and a system strips the Shield, so the compiler makes Shield
// optional in @Hull_Shield; a spawned unit must still start with it.

ent.component @Hull (hp: f32) capacity 4
ent.component @Shield ()

ent.system @make(%spawn: i1) {
  scf.if %spawn {
    %hp = arith.constant 10.0 : f32
    %id = ent.spawn (@Hull, @Shield)(%hp) : f32
  }
}

// Units with a shield gain 1 hp, once: then the shield goes.
ent.system @charge() {
  ent.query (%h: !ent.ref<@Hull, mut>, %s: !ent.ref<@Shield>) {
    %hp = ent.get %h "hp" : !ent.ref<@Hull, mut> -> f32
    %one = arith.constant 1.0 : f32
    %more = arith.addf %hp, %one : f32
    ent.set %h "hp", %more : !ent.ref<@Hull, mut>, f32
    ent.remove @Shield
  }
}

ent.schedule @frame(%spawn: i1) {
  ent.run @make(%spawn) : i1
  ent.run @charge()
}
