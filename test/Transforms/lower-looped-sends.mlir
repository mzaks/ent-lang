// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s --check-prefix=ONE

// What a query sends from a loop of its body runs once for each turn of
// the loop. A loop on one thread combines each value where it is sent.
// A parallel one visits the rows in 256 slices, each one after another
// on a thread, and puts what is sent on the slice's list; the query's
// end reads the lists in order and combines: an apply's from many values
// on by groups of the entities sent to, the groups on many threads.

ent.component @H (hp: f32, n: i32)
ent.archetype @A (@H) capacity 100000
ent.resource @Score (points: f32)

// CHECK-LABEL: func.func private @sends(
// CHECK:       scf.parallel (%[[SLICE:.*]]) = (%{{.*}}) to (%c256)
// CHECK:         scf.for %[[ROW:.*]] =
// CHECK:           scf.for
// CHECK:             func.call @ent_sends_put(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (i64, i64, i64, i64, i64) -> ()
// CHECK:             func.call @ent_sends_put(
// CHECK:       %[[SENT:.*]] = call @ent_sends_group(
// CHECK:       scf.if
// CHECK:         scf.parallel (%[[GROUP:.*]]) = (%{{.*}}) to (%c256{{.*}})
// CHECK:           call @ent_sends_group_start(
// CHECK:           call @ent_sends_group_start(
// CHECK:           scf.for
// CHECK:             call @ent_sends_id_at(
// CHECK:             call @ent_sends_value_at(
// CHECK:       } else {
// CHECK:         scf.for
// CHECK:           call @ent_sends_id_at(
// CHECK:           call @ent_sends_value_at(
// CHECK:       call @ent_sends_clear(
// CHECK:       call @ent_sends_count(
// CHECK:       scf.for
// CHECK:         call @ent_sends_value(
// CHECK:       call @ent_sends_clear(

// ONE-LABEL: func.func private @sends(
// ONE-NOT:   ent_sends
// ONE:       scf.for
// ONE:         scf.for
// ONE-NOT:   ent_sends
// ONE:       return
ent.system @sends(%d: f32) {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    %n = ent.get %h "n" : <@H> -> i32
    %to = arith.index_cast %n : i32 to index
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    scf.for %i = %c0 to %to step %c1 {
      ent.apply %id @H "hp" add %d : f32
      ent.accumulate @Score "points" add %d : f32
    }
  }
}
