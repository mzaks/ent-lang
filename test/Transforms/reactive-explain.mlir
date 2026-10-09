// RUN: ent-opt %s "--ent-lower-to-loops=explain=1" -verify-diagnostics -o /dev/null

// Reactive queries walk their triggers' event logs, except where the log
// order (the order events happened) would change the result.
ent.component @H (hp: f32)
ent.component @T (entity: !ent.entity)
ent.component @G (g: f32)
ent.archetype @A (@H, @T, @G) capacity 8

ent.system @hurt(%d: f32) writes [@H, @G] {
  ent.query (%h: !ent.ref<@H, mut>, %g: !ent.ref<@G, mut>) {
    ent.set %h "hp", %d : !ent.ref<@H, mut>, f32
    ent.set %g "g", %d : !ent.ref<@G, mut>, f32
  }
}

ent.system @pass(%d: f32) reads [@T, @G] writes [@H] {
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, connects edges or appends rows, which are combined in row order}}
  ent.query (%t: !ent.ref<@T>) on [changed @G] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
  }
}

ent.system @cull() reads [@H] writes [@A] {
  // expected-remark @+1 {{scans every entity on each run: it changes which entities archetypes hold, which is applied in row order}}
  ent.query (%h: !ent.ref<@H>) on [changed @H "hp"] {
    ent.despawn
  }
}
