// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

ent.component @P (x: f32)
ent.component @Enemy ()
ent.component @Shield ()
ent.component @Fire ()
ent.component @Ice ()
ent.resource @Paused (value: i1)
ent.resource @Clock (frame: i64)
ent.archetype @A (@P, @Enemy, optional @Shield, optional @Fire) capacity 8

// Filters print after the bindings, in the order with, without, any.
// CHECK-LABEL: ent.system @filtered() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@P>) with [@Enemy] without [@Shield] any [@Fire, @Ice] {
// CHECK:     %{{.*}} = ent.has @Shield
// CHECK:   ent.query () with [@Enemy] {
// CHECK:   ent.query () without [@Shield] {
// CHECK:   ent.query () any [@Fire, @Ice] any [@Shield, @Enemy] {
ent.system @filtered() {
  ent.query (%p: !ent.ref<@P>) with [@Enemy] without [@Shield]
      any [@Fire, @Ice] {
    %shielded = ent.has @Shield
  }
  ent.query () with [@Enemy] {
  }
  ent.query () without [@Shield] {
  }
  ent.query () any [@Fire, @Ice] any [@Shield, @Enemy] {
  }
}

// A schedule's condition takes its parameters; a run's uses them directly.
// CHECK-LABEL: ent.schedule @frame(%{{.*}}: i64) if {
// CHECK-NEXT: ^bb0(%[[N:.*]]: i64):
// CHECK-NEXT:   %[[P:.*]] = ent.read @Paused "value" : i1
// CHECK:        ent.yield %{{.*}} : i1
// CHECK-NEXT: } {
// CHECK-NEXT:   ent.run @filtered() if {
// CHECK-NEXT:     %[[F:.*]] = ent.read @Clock "frame" : i64
// CHECK-NEXT:     %[[C:.*]] = arith.cmpi slt, %[[F]], %{{.*}} : i64
// CHECK-NEXT:     ent.yield %[[C]] : i1
// CHECK-NEXT:   }
// CHECK-NEXT:   ent.run @filtered()
// CHECK-NEXT: }
ent.schedule @frame(%n: i64) if {
^bb0(%m: i64):
  %paused = ent.read @Paused "value" : i1
  %true = arith.constant true
  %run = arith.xori %paused, %true : i1
  ent.yield %run : i1
} {
  ent.run @filtered() if {
    %frame = ent.read @Clock "frame" : i64
    %early = arith.cmpi slt, %frame, %n : i64
    ent.yield %early : i1
  }
  ent.run @filtered()
}
