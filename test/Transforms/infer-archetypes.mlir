// RUN: ecs-opt %s --ecs-infer-archetypes | FileCheck %s
// Inference is idempotent.
// RUN: ecs-opt %s --ecs-infer-archetypes --ecs-infer-archetypes | FileCheck %s

module attributes {ecs.default_capacity = 7 : i64} {

ecs.component @Position (x: f32) capacity 1000
ecs.component @Velocity (dx: f32) capacity 500
ecs.component @Stunned (seconds: f32) capacity 10
ecs.component @Shield ()
ecs.component @Gun (period: f32)
ecs.component @Tag ()

// A declared archetype whose required components are exactly a spawn's is
// used for it, under its own name and capacity.
ecs.archetype @Turret (@Position, @Gun) capacity 4

// Every spawn shape gets one archetype, named after its components in
// declaration order. Stunned, which a query adds to these entities, and
// Shield, which one removes, are optional: entities never move between
// inferred archetypes. Optional components do not bound the capacity:
// it is the smallest among Position (1000) and Velocity (500).
// CHECK:      ecs.archetype @Turret (@Position, @Gun) capacity 4
// CHECK-NEXT: ecs.archetype @Position_Velocity_Shield (@Position, @Velocity, optional @Stunned, optional @Shield) capacity 500 inferred
// A shape nothing changes has no optional components. Its name would be the
// component's, so it gets a suffix; and with no capacity of its own, the
// module's default applies.
// CHECK-NEXT: ecs.archetype @Tag_archetype (@Tag) capacity 7 inferred
  ecs.system @make(%x: f32) {
    // CHECK: ecs.spawn (@Velocity, @Position, @Shield) into @Position_Velocity_Shield
    ecs.spawn (@Velocity, @Position, @Shield)(%x, %x) : f32, f32
    // CHECK: ecs.spawn (@Position, @Gun) into @Turret
    ecs.spawn (@Position, @Gun)(%x, %x) : f32, f32
    // CHECK: ecs.spawn (@Tag) into @Tag_archetype
    ecs.spawn (@Tag)()
  }
  ecs.system @hurt(%t: f32) {
    ecs.query (%v: !ecs.ref<@Velocity>) {
      ecs.add @Stunned(%t) : f32
      ecs.remove @Shield
    }
  }

}
