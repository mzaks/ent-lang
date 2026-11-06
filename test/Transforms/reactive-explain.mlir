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

// (Two that send fractions to one field: their order shows, op by op.)
ent.system @pass(%d: f32) reads [@T, @G] writes [@H] {
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, connects edges or appends rows, which are combined in row order}}
  ent.query (%t: !ent.ref<@T>) on [changed @G] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
    ent.apply %id @H "hp" add %d : f32
  }
}

// (Despawns walk the logs: the rows are gathered and visited in their
// order, so what is pending is in the order a scan gives.)
ent.system @cull() reads [@H] writes [@A] {
  ent.query (%h: !ent.ref<@H>) on [changed @H "hp"] {
    ent.despawn
  }
}

ent.buffer @Log (hp: f32) capacity 64

// (A row given a buffer walks the logs too, given where it is sent in
// the order of the rows; two ops giving rows to one buffer do not: each
// op's rows land before the next's.)
ent.system @note() reads [@H] writes [@Log] {
  ent.query (%h: !ent.ref<@H>) on [changed @H "hp"] {
    %v = ent.get %h "hp" : !ent.ref<@H> -> f32
    ent.append @Log (%v) : f32
  }
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, connects edges or appends rows, which are combined in row order}}
  ent.query (%h: !ent.ref<@H>) on [changed @H "hp"] {
    %v = ent.get %h "hp" : !ent.ref<@H> -> f32
    ent.append @Log (%v) : f32
    ent.append @Log (%v) : f32
  }
}
