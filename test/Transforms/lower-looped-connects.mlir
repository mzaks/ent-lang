// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s

// An edge connected in a loop of a query's body has no place of one for
// each row: its two ends and its field's value are put on a list as it
// is connected (the first, in a loop on one thread), and the query's end
// reads the list and adds the edges.

ent.component @N (v: f32)
ent.archetype @A (@N) capacity 100
ent.relation @R (w: i32) capacity 400

// CHECK-LABEL: func.func private @s(
// CHECK:       scf.for
// CHECK:         scf.for
// CHECK:           func.call @ent_sends_put(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (i64, i64, i64, i64) -> ()
// CHECK:           func.call @ent_sends_put(
// CHECK:       call @ent_sends_count(
// CHECK:       scf.for
// CHECK:         call @ent_sends_id(
// CHECK:         call @ent_sends_value(
// CHECK:         call @ent_sends_id(
// CHECK:         call @ent_sends_value(
// CHECK:       call @ent_sends_clear(
ent.system @s(%w: i32) reads [@N] writes [@R] {
  ent.query (%n: !ent.ref<@N>) {
    %self = ent.entity
    %c = arith.constant 0 : index
    %one = arith.constant 1 : index
    %three = arith.constant 3 : index
    scf.for %i = %c to %three step %one {
      ent.connect @R %self, %self (%w) : i32
    }
  }
}
