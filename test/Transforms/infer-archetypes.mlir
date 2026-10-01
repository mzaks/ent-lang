// RUN: ent-opt %s --ent-infer-archetypes | FileCheck %s
// Inference is idempotent.
// RUN: ent-opt %s --ent-infer-archetypes --ent-infer-archetypes | FileCheck %s

module attributes {ent.default_capacity = 7 : i64} {

ent.component @Position (x: f32) capacity 1000
ent.component @Velocity (dx: f32) capacity 500
ent.component @Stunned (seconds: f32) capacity 10
ent.component @Shield ()
ent.component @Gun (period: f32)
ent.component @Tag ()

// A declared archetype whose required components are exactly a spawn's is
// used for it, under its own name and capacity.
ent.archetype @Turret (@Position, @Gun) capacity 4

// Every spawn shape gets one archetype, named after its components in
// declaration order. Stunned, which a query adds to these entities, and
// Shield, which one removes, are optional: entities never move between
// inferred archetypes. Optional components do not bound the capacity:
// it is the smallest among Position (1000) and Velocity (500).
// CHECK:      ent.archetype @Turret (@Position, @Gun) capacity 4
// CHECK-NEXT: ent.archetype @Position_Velocity_Shield (@Position, @Velocity, optional @Stunned, optional @Shield) capacity 500 inferred
// A shape nothing changes has no optional components. Its name would be the
// component's, so it gets a suffix; and with no capacity of its own, the
// module's default applies.
// CHECK-NEXT: ent.archetype @Tag_archetype (@Tag) capacity 7 inferred
  ent.system @make(%x: f32) {
    // CHECK: ent.spawn (@Velocity, @Position, @Shield) into @Position_Velocity_Shield
    ent.spawn (@Velocity, @Position, @Shield)(%x, %x) : f32, f32
    // CHECK: ent.spawn (@Position, @Gun) into @Turret
    ent.spawn (@Position, @Gun)(%x, %x) : f32, f32
    // CHECK: ent.spawn (@Tag) into @Tag_archetype
    ent.spawn (@Tag)()
  }
  ent.system @hurt(%t: f32) {
    ent.query (%v: !ent.ref<@Velocity>) {
      ent.add @Stunned(%t) : f32
      ent.remove @Shield
    }
  }

}
