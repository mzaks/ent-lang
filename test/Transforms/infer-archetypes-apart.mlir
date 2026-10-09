// RUN: ent-opt %s --ent-infer-archetypes | FileCheck %s
// Inference is idempotent.
// RUN: ent-opt %s --ent-infer-archetypes --ent-infer-archetypes | FileCheck %s

// The entities with a component declared `apart` are stored apart from
// those without it: no archetype holds it optionally, and for every
// archetype whose entities are given it or lose it there is another, with
// it or without, that they move to.

module attributes {ent.default_capacity = 7 : i64} {

ent.component @Position (x: f32) capacity 1000
ent.component @Foe (hp: f32) capacity 500
ent.component @Close () apart capacity 100
ent.component @Stunned (seconds: f32)
ent.component @Gun (period: f32)
ent.component @Jammed () apart
ent.component @Gem ()
ent.component @Shown () apart

// A declared archetype's entities move too, to one that is inferred and
// named after it: what it holds optionally, that one does, and it is no
// bigger.
ent.archetype @Turret (@Position, @Gun, optional @Stunned) capacity 4

// CHECK:      ent.archetype @Turret (@Position, @Gun, optional @Stunned) capacity 4
// CHECK-NEXT: ent.archetype @Turret_Jammed (@Position, optional @Stunned, @Gun, @Jammed) capacity 4 inferred
// A spawn's shape, and the same with Close. Stunned, which only the close
// ones are given, is optional in both: one that is stunned and then no
// longer close is still stunned. Close bounds the capacity where it is
// required.
// CHECK-NEXT: ent.archetype @Position_Foe (@Position, @Foe, optional @Stunned) capacity 500 inferred
// CHECK-NEXT: ent.archetype @Position_Foe_Close (@Position, @Foe, @Close, optional @Stunned) capacity 100 inferred
// Two shapes that are spawned, and one is what the other's entities move
// to: one archetype for each, not a second for the second spawn.
// CHECK-NEXT: ent.archetype @Gem_Shown (@Gem, @Shown) capacity 7 inferred
// CHECK-NEXT: ent.archetype @Gem_archetype (@Gem) capacity 7 inferred
// CHECK-NOT:  ent.archetype
  ent.system @make(%x: f32) {
    // CHECK: ent.spawn (@Position, @Foe) into @Position_Foe
    ent.spawn (@Position, @Foe)(%x, %x) : f32, f32
    // CHECK: ent.spawn (@Position, @Gun) into @Turret
    ent.spawn (@Position, @Gun)(%x, %x) : f32, f32
    // CHECK: ent.spawn (@Gem, @Shown) into @Gem_Shown
    ent.spawn (@Gem, @Shown)()
    // CHECK: ent.spawn (@Gem) into @Gem_archetype
    ent.spawn (@Gem)()
  }
  ent.system @gather(%t: f32) {
    ent.query (%f: !ent.ref<@Foe>) without [@Close] {
      ent.add @Close()
    }
    ent.query (%f: !ent.ref<@Foe>) with [@Close] {
      ent.remove @Close
    }
    ent.query (%f: !ent.ref<@Foe>) with [@Close] {
      ent.add @Stunned(%t) : f32
    }
    ent.query (%g: !ent.ref<@Gun>) {
      ent.add @Jammed()
    }
    ent.query () with [@Gem] {
      ent.remove @Shown
    }
  }

}
