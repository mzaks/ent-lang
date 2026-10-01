// RUN: ecs-opt %s "--ecs-lower-to-loops=explain=1" -verify-diagnostics -o /dev/null

// Reactive queries walk their triggers' event logs, except where the log
// order (the order events happened) would change the result.
ecs.component @H (hp: f32)
ecs.component @T (entity: !ecs.entity)
ecs.component @G (g: f32)
ecs.archetype @A (@H, @T, @G) capacity 8

ecs.system @hurt(%d: f32) writes [@H, @G] {
  ecs.query (%h: !ecs.ref<@H, mut>, %g: !ecs.ref<@G, mut>) {
    ecs.set %h "hp", %d : !ecs.ref<@H, mut>, f32
    ecs.set %g "g", %d : !ecs.ref<@G, mut>, f32
  }
}

ecs.system @pass(%d: f32) reads [@T, @G] writes [@H] {
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, which are combined in row order}}
  ecs.query (%t: !ecs.ref<@T>) on [changed @G] {
    %id = ecs.get %t "entity" : !ecs.ref<@T> -> !ecs.entity
    ecs.apply %id @H "hp" add %d : f32
  }
}

ecs.system @cull() reads [@H] writes [@A] {
  // expected-remark @+1 {{scans every entity on each run: it changes which entities archetypes hold, which is applied in row order}}
  ecs.query (%h: !ecs.ref<@H>) on [changed @H "hp"] {
    ecs.despawn
  }
}
