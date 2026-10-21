// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR

// An edge connected in a loop of a query's body has no place of one for
// each row: its two ends and its field's value are put on a list as it
// is connected (the first, in a loop on one thread), and the query's end
// reads the list and adds the edges. Where the query ran on many
// threads, from many edges on, each slice of the list writes its edges on
// a thread, where those of the slices before it end.

ent.component @N (v: f32)
ent.archetype @A (@N) capacity 100
ent.relation @R (w: i32) capacity 400

// CHECK-LABEL: func.func private @s(
// CHECK:       scf.for
// CHECK:         scf.for
// CHECK:           func.call @ent_sends_put(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (i64, i64, i64, i64, i64) -> ()
// CHECK:           func.call @ent_sends_put(
// CHECK:       call @ent_sends_count(
// CHECK:       scf.for
// CHECK:         call @ent_sends_id(
// CHECK:         call @ent_sends_value(
// CHECK:         call @ent_sends_id(
// CHECK:         call @ent_sends_value(
// CHECK:       call @ent_sends_clear(
// PAR-LABEL: func.func private @s(
// PAR:       scf.parallel
// PAR:           func.call @ent_sends_put(
// PAR:       %[[SENT:.*]] = call @ent_sends_count(
// PAR:       scf.if
// PAR:         call @ent_sends_slices(
// PAR:         cf.assert {{.*}} "ent.connect exceeds the capacity of @R"
// PAR:         scf.parallel
// PAR:           call @ent_sends_slice_before(
// PAR:           call @ent_sends_slice_before(
// PAR:           call @ent_sends_slice_first(
// PAR:           scf.for
// PAR:             call @ent_sends_slice_id(
// PAR:             call @ent_sends_slice_value(
// PAR:             call @ent_sends_slice_id(
// PAR:             call @ent_sends_slice_value(
// PAR:       } else {
// PAR:         scf.for
// PAR:           call @ent_sends_id(
// PAR:       call @ent_sends_clear(
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
