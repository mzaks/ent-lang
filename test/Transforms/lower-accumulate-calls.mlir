// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1000" \
// RUN:   | FileCheck %s

// A loop whose body calls out is worth running in parallel from fewer
// entities on than one that only computes: these 100 are too few for
// @plain and enough for @calls. What @calls adds into @Sum is therefore
// not added where it stands, a load and a store of the one cell from
// every thread, but kept for each row and added up after the loop, as
// in any parallel loop.

ent.component @P (x: f32)
ent.archetype @Ps (@P) capacity 100
ent.resource @Sum (value: f32)
ent.function @noise(f32) -> f32

// CHECK-LABEL: func.func private @plain(
// CHECK-NOT:   scf.parallel
// CHECK:       scf.for
// CHECK:       return
ent.system @plain() {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : <@P> -> f32
    ent.accumulate @Sum "value" add %x : f32
  }
}

// CHECK-LABEL: func.func private @calls(
// CHECK:       scf.parallel (%[[ROW:.*]]) =
// CHECK:         %[[N:.*]] = func.call @ent_noise(
// CHECK-NOT:     memref<1xf32>
// CHECK:         memref.store %[[N]], %{{.*}}[%[[ROW]]] : memref<100xf32>
// CHECK:         scf.reduce
ent.system @calls() {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : <@P> -> f32
    %n = ent.invoke @noise(%x) : (f32) -> f32
    ent.accumulate @Sum "value" add %n : f32
  }
}
