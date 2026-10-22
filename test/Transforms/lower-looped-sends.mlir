// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s --check-prefix=ONE

// What a query sends from a loop of its body runs once for each turn of
// the loop. A loop on one thread combines each value where it is sent.
// A parallel one visits the rows in 256 slices, each one after another
// on a thread, and puts what is sent on the slice's list (asked for where
// the slice starts, and written to without a call, but where its block
// is full or there is none). What an apply sends to entities goes on 64
// lists, by the group of the target's row (its high bits), each slice's
// in a table where it starts. The query's end reads the lists in order,
// where they are (a block at a time), and combines: an apply's from many
// values on, where that has taken less so far (devices/sends.c chooses),
// a group's lists on each thread, at once, each counted first and emptied
// after on its thread.

ent.component @H (hp: f32, n: i32)
ent.archetype @A (@H) capacity 100000
ent.resource @Score (points: f32)

// CHECK-LABEL: func.func private @sends(
// CHECK:       scf.parallel (%[[SLICE:.*]]) = (%{{.*}}) to (%c256)
// CHECK:         %[[SECOND:.*]] = func.call @ent_sends_slice_list(%{{.*}}, %c64_i64{{.*}}, %{{.*}}, %c2_i64{{.*}}) : (i64, i64, i64, i64) -> i64
// CHECK:         %[[GROUPS:.*]] = memref.alloca() : memref<64xi64>
// CHECK:         scf.for %[[GROUP:.*]] = %{{.*}} to %c64
// CHECK:           %[[LIST:.*]] = func.call @ent_sends_slice_list(
// CHECK:           memref.store %[[LIST]], %[[GROUPS]][%[[GROUP]]]
// CHECK:         scf.for %[[ROW:.*]] =
// CHECK:           scf.for
// CHECK:             arith.andi %{{.*}}, %c131071_i64
// CHECK:             arith.shrui %{{.*}}, %c11_i64
// CHECK:             %[[FIRST:.*]] = memref.load %[[GROUPS]]
// CHECK:             func.call @ent_sends_slice_grow(%{{.*}}, %[[FIRST]])
// CHECK:             llvm.store
// CHECK:             func.call @ent_sends_slice_grow(%{{.*}}, %[[SECOND]])
// CHECK:             llvm.store
// CHECK:       scf.parallel ({{.*}}) = ({{.*}}) to (%c64{{.*}}) step ({{.*}}) init
// CHECK:         %[[COUNTED:.*]] = func.call @ent_sends_count(
// CHECK:         scf.reduce(%[[COUNTED]] : i64)
// CHECK:           arith.addi
// CHECK:       call @ent_sends_choose({{.*}}, %c16384_i64, %c131072_i64)
// CHECK:       scf.if
// CHECK:         scf.parallel ({{.*}}) = ({{.*}}) to (%c64
// CHECK:           scf.while
// CHECK:             call @ent_sends_next_block(
// CHECK:           call @ent_sends_clear(
// CHECK:       } else {
// CHECK:         scf.for {{.*}} to %c64
// CHECK:           scf.while
// CHECK:             call @ent_sends_next_block(
// CHECK:           call @ent_sends_clear(
// CHECK:       call @ent_sends_chosen(
// CHECK:       call @ent_sends_count(
// CHECK:       scf.while
// CHECK:         call @ent_sends_next_block(
// CHECK:       } do {
// CHECK:         scf.for {{.*}} iter_args
// CHECK:           llvm.load
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
