// RUN: ecs-opt %s -split-input-file --ecs-infer-archetypes -verify-diagnostics

ecs.component @Position (x: f32)
ecs.system @make(%x: f32) {
  // expected-error @+1 {{spawns into an archetype (@Position_archetype) with no capacity: give one of its required components a capacity, or set ecs.default_capacity on the module}}
  ecs.spawn (@Position)(%x) : f32
}

// -----

ecs.component @Position (x: f32) capacity 8
ecs.component @Shield () capacity 8
ecs.component @Velocity (dx: f32)
// Removing Shield makes it optional, so its capacity no longer bounds the
// archetype, and Velocity, the one required component left, has none.
ecs.system @make(%x: f32) {
  // expected-error @+1 {{spawns into an archetype (@Shield_Velocity) with no capacity}}
  ecs.spawn (@Velocity, @Shield)(%x) : f32
}
ecs.system @strip() {
  ecs.query (%v: !ecs.ref<@Velocity>) {
    ecs.remove @Shield
  }
}

// -----

// A despawn from an inferred archetype under a contract lists the query's
// components in `writes`.
ecs.component @Position (x: f32) capacity 8
ecs.archetype @Position_archetype (@Position) capacity 8 inferred
ecs.system @cull() reads [@Position] {
  ecs.query (%p: !ecs.ref<@Position>) {
    // expected-error @+1 {{despawns entities with @Position but system @cull does not declare it in 'writes'}}
    ecs.despawn
  }
}
