// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s

// Whole numbers that a parallel loop adds into a resource, or keeps the
// least of, are reduced: the cell is read before the loop, every
// iteration gives the loop what it comes to (from the number that changes
// nothing, where a condition keeps it from sending), and the cell is
// written after it. Nothing is kept for each row. A float's least is
// reduced too, as a whole number of its bits in the same order (a NaN the
// least); a float's sum is kept for each row: it depends on the order.

ent.component @P (n: i32, x: f32)
ent.archetype @Ps (@P) capacity 100000
ent.resource @Sum (count: i32, least: i32, part: f32, low: f32)

// CHECK-LABEL: func.func private @whole(
// CHECK:       %[[COUNT:.*]] = memref.load %[[CELL:.*]][%{{.*}}] : memref<1xi32>
// CHECK:       %[[LEAST:.*]] = memref.load %[[LOW:.*]][%{{.*}}] : memref<1xi32>
// CHECK:       %[[SUMS:.*]]:2 = scf.parallel (%[[ROW:.*]]) = {{.*}} init (%[[COUNT]], %[[LEAST]]) -> (i32, i32) {
// CHECK-NOT:     memref.store
// CHECK:         scf.reduce(%{{.*}}, %{{.*}} : i32, i32) {
// CHECK:           arith.addi
// CHECK:           scf.reduce.return
// CHECK:           arith.cmpi slt
// CHECK:           arith.select
// CHECK:           scf.reduce.return
// CHECK:       memref.store %[[SUMS]]#0, %[[CELL]][
// CHECK:       memref.store %[[SUMS]]#1, %[[LOW]][
ent.system @whole() {
  ent.query (%p: !ent.ref<@P>) {
    %n = ent.get %p "n" : <@P> -> i32
    ent.accumulate @Sum "count" add %n : i32
    %zero = arith.constant 0 : i32
    %some = arith.cmpi sgt, %n, %zero : i32
    scf.if %some {
      ent.accumulate @Sum "least" min %n : i32
    }
  }
}

// CHECK-LABEL: func.func private @float(
// CHECK:       scf.parallel (%[[ROW:.*]]) =
// CHECK:         memref.store %{{.*}}, %{{.*}}[%[[ROW]]] : memref<100000xf32>
// CHECK:         scf.reduce
ent.system @float() {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : <@P> -> f32
    ent.accumulate @Sum "part" add %x : f32
  }
}

// CHECK-LABEL: func.func private @floatLeast(
// CHECK:       %[[LOW:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<1xf32>
// CHECK:       arith.bitcast %[[LOW]] : f32 to i32
// CHECK:       scf.parallel (%{{.*}}) = {{.*}} -> i32 {
// CHECK-NOT:     memref.store
// CHECK:         arith.minimumf
// CHECK:         arith.bitcast %{{.*}} : f32 to i32
// CHECK:         scf.reduce(%{{.*}} : i32) {
// CHECK:           arith.cmpi slt
// CHECK:           arith.select
// CHECK:       arith.bitcast %{{.*}} : i32 to f32
ent.system @floatLeast() {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : <@P> -> f32
    ent.accumulate @Sum "low" min %x : f32
  }
}
