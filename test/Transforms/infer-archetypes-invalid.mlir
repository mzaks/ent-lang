// RUN: ent-opt %s -split-input-file --ent-infer-archetypes -verify-diagnostics

ent.component @Position (x: f32)
ent.system @make(%x: f32) {
  // expected-error @+1 {{spawns into an archetype (@Position_archetype) with no capacity: give one of its required components a capacity, or set ent.default_capacity on the module}}
  ent.spawn (@Position)(%x) : f32
}

// -----

ent.component @Position (x: f32) capacity 8
ent.component @Shield () capacity 8
ent.component @Velocity (dx: f32)
// Removing Shield makes it optional, so its capacity no longer bounds the
// archetype, and Velocity, the one required component left, has none.
ent.system @make(%x: f32) {
  // expected-error @+1 {{spawns into an archetype (@Shield_Velocity) with no capacity}}
  ent.spawn (@Velocity, @Shield)(%x) : f32
}
ent.system @strip() {
  ent.query (%v: !ent.ref<@Velocity>) {
    ent.remove @Shield
  }
}

// -----

// A despawn from an inferred archetype under a contract lists the query's
// components in `writes`.
ent.component @Position (x: f32) capacity 8
ent.archetype @Position_archetype (@Position) capacity 8 inferred
ent.system @cull() reads [@Position] {
  ent.query (%p: !ent.ref<@Position>) {
    // expected-error @+1 {{despawns entities with @Position but system @cull does not declare it in 'writes'}}
    ent.despawn
  }
}
