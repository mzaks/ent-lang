// RUN: ecs-opt %s --ecs-lower-to-loops -verify-diagnostics -o /dev/null

ecs.component @H (hp: f32, max: f32)
ecs.component @S ()
ecs.archetype @A (@H, optional @S) capacity 8

// Clamping hp in a query that reacts to hp collects the clamped entities
// again on the next run.
ecs.system @clamp() writes [@H] {
  // expected-warning @+1 {{reacts to changed @H "hp", which it causes itself: every run collects the entities it changed for the next run. Consider a system that runs every frame, splitting this one, or a marker component}}
  ecs.query (%h: !ecs.ref<@H, mut>) on [changed @H "hp"] {
    %x = ecs.get %h "hp" : !ecs.ref<@H, mut> -> f32
    // expected-note @+1 {{causes changed @H "hp"}}
    ecs.set %h "hp", %x : !ecs.ref<@H, mut>, f32
  }
}

// Nothing writes "max" or removes @S.
ecs.system @watch() reads [@H, @S] {
  // expected-warning @+2 {{reacts to changed @H "max", but no system writes it; this trigger fires only for entities spawned with it or gaining it}}
  // expected-warning @+1 {{reacts to removed @S, but no system removes it; this trigger never fires}}
  ecs.query (%h: !ecs.ref<@H>) on [changed @H "max", removed @S] {
  }
}
