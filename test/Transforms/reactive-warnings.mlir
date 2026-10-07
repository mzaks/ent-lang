// RUN: ent-opt %s --ent-lower-to-loops -verify-diagnostics -o /dev/null

ent.component @H (hp: f32, max: f32)
ent.component @S ()
ent.archetype @A (@H, optional @S) capacity 8

// Clamping hp in a query that reacts to hp collects the clamped entities
// again on the next run.
ent.system @clamp() writes [@H] {
  // expected-warning @+1 {{reacts to changed @H "hp", which it causes itself: every run collects the entities it changed for the next run. Consider a system that runs every frame, splitting this one, or a marker component}}
  ent.query (%h: !ent.ref<@H, mut>) on [changed @H "hp"] {
    %x = ent.get %h "hp" : !ent.ref<@H, mut> -> f32
    // expected-note @+1 {{causes changed @H "hp"}}
    ent.set %h "hp", %x : !ent.ref<@H, mut>, f32
  }
}

// Nothing writes "max" or removes @S.
ent.system @watch() reads [@H, @S] {
  // expected-warning @+2 {{reacts to changed @H "max", but no system writes it; this trigger fires only for entities spawned with it or gaining it}}
  // expected-warning @+1 {{reacts to removed @S, but no system removes it; this trigger never fires}}
  ent.query (%h: !ent.ref<@H>) on [changed @H "max", removed @S] {
  }
}

// A module's query (its system is `module.name`) reacts to what the
// program that imports it may do or not: it is not told.
ent.system @lib.watch() reads [@H, @S] {
  ent.query (%h: !ent.ref<@H>) on [changed @H "max", removed @S] {
  }
}
