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

// -----

// The archetype the entities move to as they lose a component that is
// `apart` needs a capacity as any other: Close, which bounds how many
// there are with it, says nothing of those without.
ent.component @Position (x: f32)
ent.component @Close () apart capacity 8
ent.system @make(%x: f32) {
  ent.spawn (@Position, @Close)(%x) : f32
}
ent.system @gather() {
  ent.query (%p: !ent.ref<@Position>) with [@Close] {
    // expected-error @+1 {{moves entities to an archetype (@Position_archetype) with no capacity}}
    ent.remove @Close
  }
}

// -----

// A declared archetype in the way of the one to move to: the same
// required components, and another that it holds optionally.
ent.component @Position (x: f32) capacity 8
ent.component @Close () apart
ent.component @Lit ()
ent.archetype @Far (@Position) capacity 8
ent.archetype @Near (@Position, @Close, optional @Lit) capacity 8
ent.system @gather() {
  ent.query (%p: !ent.ref<@Position>) without [@Close] {
    // expected-error @+1 {{adds @Close to entities of @Far, but the archetype they would move to holds other components optionally than @Far does}}
    ent.add @Close()
  }
}
