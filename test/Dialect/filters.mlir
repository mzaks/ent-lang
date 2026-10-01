// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

ent.component @P (x: f32)
ent.component @Enemy ()
ent.component @Shield ()
ent.component @Fire ()
ent.component @Ice ()
ent.archetype @A (@P, @Enemy, optional @Shield, optional @Fire) capacity 8

// Filters print after the bindings, in the order with, without, any.
// CHECK-LABEL: ent.system @filtered() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@P>) with [@Enemy] without [@Shield] any [@Fire, @Ice] {
// CHECK:   ent.query () with [@Enemy] {
// CHECK:   ent.query () without [@Shield] {
// CHECK:   ent.query () any [@Fire, @Ice] any [@Shield, @Enemy] {
ent.system @filtered() {
  ent.query (%p: !ent.ref<@P>) with [@Enemy] without [@Shield]
      any [@Fire, @Ice] {
  }
  ent.query () with [@Enemy] {
  }
  ent.query () without [@Shield] {
  }
  ent.query () any [@Fire, @Ice] any [@Shield, @Enemy] {
  }
}
