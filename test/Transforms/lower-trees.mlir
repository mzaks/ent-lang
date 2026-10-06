// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-print-access 2>&1 | FileCheck %s --check-prefix=ACCESS
// Reading up a tree is entity-local, so such a loop may run in parallel;
// a cascading one has its order.
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:     --convert-scf-to-openmp --canonicalize --ent-omp-nowait \
// RUN:     --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts \
// RUN:   | mlir-translate --mlir-to-llvmir -o /dev/null

ent.component @Node (local: i32, total: i32)
ent.component @Mark (color: i32)
ent.relation @Under () from @Node to @Node tree capacity 16
ent.archetype @Plain (@Node) capacity 8
ent.archetype @Marked (@Node, @Mark) capacity 8

// A sum down the tree. Nothing despawns a @Node, so the parent, the other
// end of the entity's one edge, is known to be alive and to have it: no
// search up the tree. Entities with a parent come in the order the sort
// left, each found by its id; with a ref up the tree the query cascades
// along, those without a parent are not visited at all.
// ACCESS: remark: reads Plain.Node.total, Marked.Node.total, Plain.Node.local, Marked.Node.local, Plain.count, Marked.count, Under.edges, entities, Plain.id, Marked.id; writes Plain.Node.total, Marked.Node.total
// PAR-LABEL:   func.func private @sum(
// PAR-NOT:     scf.parallel
// PAR:         return
// CHECK-LABEL: func.func private @sum(
// CHECK-NOT:   scf.while
// CHECK:       %[[N:.*]] = memref.load %[[COUNT:.*]][%{{.*}}] : memref<1xi64>
// CHECK:       %[[END:.*]] = arith.index_cast %[[N]] : i64 to index
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to %[[END]] step %{{.*}} {
// CHECK-NEXT:    %[[ID:.*]] = memref.load %[[ORDER:.*]][%[[I]]] : memref<16xi32>
// CHECK:         scf.if
// CHECK-NOT:   scf.while
// CHECK:       return
ent.system @sum() {
  ent.query (%n: !ent.ref<@Node, mut>, %p: !ent.ref<@Node, up @Under>)
      cascade @Under {
    %above = ent.get %p "total" : !ent.ref<@Node, up @Under> -> i32
    %own = ent.get %n "local" : !ent.ref<@Node, mut> -> i32
    %total = arith.addi %above, %own : i32
    ent.set %n "total", %total : !ent.ref<@Node, mut>, i32
  }
}

// The nearest ancestor with a @Mark may be any number of edges up: a
// search, per entity, and no order, so a loop per archetype.
// ACCESS: remark: reads Marked.Mark.color, Plain.count, Under.edges, entities, Plain.id; writes Plain.Node.total
// PAR-LABEL:   func.func private @inherit(
// PAR:         scf.parallel
// PAR:           scf.while
// PAR-LABEL:   func.func private @visit(
// PAR-NOT:     scf.parallel
// PAR:         return
// CHECK-LABEL: func.func private @inherit(
// CHECK:       scf.for
// CHECK:         scf.while
// CHECK:         scf.if
// CHECK-NOT:   scf.for
// CHECK:       return
ent.system @inherit() {
  ent.query (%n: !ent.ref<@Node, mut>, %m: !ent.ref<@Mark, up @Under>)
      without [@Mark] {
    %color = ent.get %m "color" : !ent.ref<@Mark, up @Under> -> i32
    ent.set %n "total", %color : !ent.ref<@Node, mut>, i32
  }
}

// Only ordered: the entities without a parent, archetype by archetype,
// then the others.
// CHECK-LABEL: func.func private @visit(
// CHECK:       scf.for
// CHECK:       scf.for
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to
// CHECK-NEXT:    memref.load %{{.*}}[%[[I]]] : memref<16xi32>
// CHECK:       return
ent.system @visit() {
  ent.query (%n: !ent.ref<@Node, mut>) cascade @Under {
    %own = ent.get %n "local" : !ent.ref<@Node, mut> -> i32
    ent.set %n "total", %own : !ent.ref<@Node, mut>, i32
  }
}

ent.schedule @frame() {
  ent.run @sum()
  ent.run @inherit()
  ent.run @visit()
}

// Children first: the list from its end, then the entities without a
// parent; what goes to the parent is combined there at once.
// ACCESS: remark: reads Plain.Node.total, Marked.Node.total, Plain.count, Marked.count, Under.edges, entities, Plain.id, Marked.id; writes Plain.Node.total, Marked.Node.total
// CHECK-LABEL: func.func private @gather(
// CHECK:       scf.for %[[I:.*]] = %{{.*}} to %[[END:.*]] step %[[ONE:.*]] {
// CHECK-NEXT:    %[[LAST:.*]] = arith.subi %[[END]], %[[ONE]]
// CHECK-NEXT:    %[[AT:.*]] = arith.subi %[[LAST]], %[[I]]
// CHECK-NEXT:    memref.load %{{.*}}[%[[AT]]] : memref<16xi32>
// CHECK-NOT:   ent.
// CHECK:       return
ent.system @gather() {
  ent.query (%n: !ent.ref<@Node>, %d: !ent.ref<@Node, mut, up @Under>)
      cascade @Under leaves first {
    %own = ent.get %n "total" : !ent.ref<@Node> -> i32
    ent.combine %d "total" add %own : !ent.ref<@Node, mut, up @Under>, i32
  }
}
// PAR-LABEL:   func.func private @gather(
// PAR-NOT:     scf.parallel
// PAR:         return

// Sorting a tree keeps the last edge of each source, lists the entities
// with a parent breadth first, and stops if some were not reached.
// CHECK-LABEL: func.func private @ent_sort_Under(
// CHECK:       scf.while
// CHECK:       cf.assert %{{.*}}, "@Under is a tree, but an entity is its own ancestor"
