// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ent-opt %s --ent-print-access -o /dev/null 2>&1 | FileCheck %s --check-prefix=ACCESS

// A buffer is a count in the world's header and a column for each field.
// A row appended outside a query is stored at the count, which moves on.
// One appended inside a query goes to a place of the entity's own (a
// byte that says there is one, and the values), and when the query has
// run the rows are copied out in the order of the entities: so the query
// may run on all cores, and the order is the same.

module attributes {ent.default_capacity = 8 : i64} {

ent.component @At (x: f32)
ent.buffer @Shapes (kind: i32, x: f32) capacity 16
ent.function proc @draw(!ent.buffer<@Shapes>, f32)
ent.archetype @Things (@At) capacity 8

// ACCESS: remark: reads Things.At.x, Things.count, Shapes.rows; writes Shapes.rows

// The C function takes how many rows there are, where each field's
// values are, and then its other parameters.
// CHECK: func.func private @ent_draw(i32, !llvm.ptr, !llvm.ptr, f32)

// CHECK-LABEL: func.func private @fill(
// CHECK-DAG:     %[[KIND:.*]] = memref.view %{{.*}} to memref<16xi32>
// CHECK-DAG:     %[[X:.*]] = memref.view %{{.*}} to memref<16xf32>
// CHECK-DAG:     %[[SENT:.*]] = memref.view %{{.*}} to memref<8xi8>
// Emptied.
// CHECK:         %[[NO_ROWS:.*]] = arith.constant 0 : i64
// CHECK-NEXT:    memref.store %[[NO_ROWS]], %[[COUNT:.*]][%{{.*}}] : memref<1xi64>
// Outside the query.
// CHECK:         cf.assert %{{.*}}, "no room for another row: the buffer @Shapes holds 16; 'capacity' gives more"
// CHECK-NEXT:    memref.store %{{.*}}, %[[KIND]][%[[AT:.*]]] : memref<16xi32>
// CHECK-NEXT:    memref.store %{{.*}}, %[[X]][%[[AT]]] : memref<16xf32>
// Inside: under an `if`, so the place first says that there is no row.
// CHECK:         scf.for %[[ROW:.*]] = %{{.*}} to %[[ROWS:.*]] step
// CHECK-NEXT:      %[[NONE:.*]] = arith.constant 0 : i8
// CHECK-NEXT:      memref.store %[[NONE]], %[[SENT]][%[[ROW]]] : memref<8xi8>
// CHECK:           scf.if
// CHECK:             memref.store %{{.*}}, %[[SENT]][%[[ROW]]] : memref<8xi8>
// Copied out when it has run, how many there are carried round.
// CHECK:         %[[END:.*]] = scf.for %[[ROW2:.*]] = %{{.*}} to %[[ROWS]] step %{{.*}} iter_args(%[[TO:.*]] = %{{.*}}) -> (index) {
// CHECK-NEXT:      %[[IS:.*]] = memref.load %[[SENT]][%[[ROW2]]] : memref<8xi8>
// CHECK:           cf.assert
// CHECK-NEXT:      memref.store %{{.*}}, %[[KIND]][%[[TO]]] : memref<16xi32>
// CHECK:         %[[TOTAL:.*]] = arith.index_cast %[[END]] : index to i64
// CHECK-NEXT:    memref.store %[[TOTAL]], %[[COUNT]][%{{.*}}] : memref<1xi64>
// A row it has not reads as nought.
// CHECK:         %[[THERE:.*]] = arith.cmpi ult
// CHECK:         arith.select %[[THERE]], %{{.*}}, %{{.*}} : f32
// Handed over: the count as an i32, and the columns' addresses.
// CHECK:         memref.extract_aligned_pointer_as_index %[[KIND]]
// CHECK:         memref.extract_aligned_pointer_as_index %[[X]]
// CHECK:         call @ent_draw(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (i32, !llvm.ptr, !llvm.ptr, f32) -> ()

// PAR-LABEL: func.func private @fill(
// PAR:         scf.parallel
// PAR:         scf.for {{.*}} iter_args
ent.system @fill(%dx: f32) {
  ent.clear @Shapes
  %k = arith.constant 7 : i32
  ent.append @Shapes (%k, %dx) : i32, f32
  ent.query (%a: !ent.ref<@At>) {
    %x = ent.get %a "x" : !ent.ref<@At> -> f32
    %c = arith.cmpf ogt, %x, %dx : f32
    scf.if %c {
      ent.append @Shapes (%k, %x) : i32, f32
    }
  }
  %n = ent.buffer.len @Shapes
  %v = ent.buffer.at @Shapes "x"[%n : i32] : f32
  %s = ent.buffer.of @Shapes : !ent.buffer<@Shapes>
  ent.invoke proc @draw(%s, %v) : (!ent.buffer<@Shapes>, f32) -> ()
}

}
