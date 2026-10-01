// RUN: ent-opt %s --split-input-file --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --split-input-file --ent-print-access 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ACCESS

ent.component @N (v: f32, input: f32) capacity 100
ent.archetype @A (@N) capacity 100
ent.relation @Syn (w: f32) capacity 1000

// Row ids: an entity's key is its id. Edges by source at 32896 (129
// offsets: 128 keys), target ids at 22656, weights at 27776; the apply's
// per-row flags at 4224 and per-edge target ids and values at 7296 and
// 12416.
// CHECK-LABEL: func.func private @push(
// CHECK:      scf.for %[[ROW:.*]] =
// The row's flag: "did not run the loop", then "ran it".
// CHECK:        memref.store %{{.*}}, %[[FLAGS:.*]][%[[ROW]]] : memref<100xi32>
// CHECK:        %[[KEY:.*]] = arith.index_castui %{{.*}} : i32 to index
// CHECK:        memref.store %{{.*}}, %[[FLAGS]][%[[ROW]]] : memref<100xi32>
// CHECK:        %[[NEXT:.*]] = arith.addi %[[KEY]], %{{.*}} : index
// CHECK-NEXT:   %[[B:.*]] = memref.load %[[OUT:.*]][%[[KEY]]] : memref<129xi32>
// CHECK-NEXT:   %[[E:.*]] = memref.load %[[OUT]][%[[NEXT]]] : memref<129xi32>
// CHECK:        scf.for %[[P:.*]] =
// CHECK:          memref.store %{{.*}}, %[[IDS:.*]][%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     %[[T:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     %[[W:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xf32>
// CHECK-NEXT:     %[[X:.*]] = arith.mulf %{{.*}}, %[[W]] : f32
// CHECK-NEXT:     memref.store %[[T]], %[[IDS]][%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     memref.store %[[X]], %{{.*}}[%[[P]]] : memref<1000xf32>
// Combined when the query ends: rows that ran, then their edges, in order.
// CHECK:      scf.for %[[R:.*]] =
// CHECK-NEXT:   memref.load %[[FLAGS]][%[[R]]]
// CHECK:        scf.if
// CHECK:          scf.for %[[Q:.*]] =
// CHECK-NEXT:       memref.load %[[IDS]][%[[Q]]]
// CHECK:              arith.addf
// ACCESS: remark: reads A.N.v, Syn.w, entities, Syn.edges, A.id, A.count; writes A.N.input
ent.system @push() {
  ent.query (%n: !ent.ref<@N>) {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    ent.edges @Syn out (%s: !ent.ref<@Syn>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      %x = arith.mulf %v, %w : f32
      ent.apply %t @N "input" add %x : f32
    }
  }
}

// Writing edges touches only the relation's field.
// ACCESS: remark: reads Syn.w, Syn.edges, A.id, A.count; writes Syn.w
ent.system @learn() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn out (%s: !ent.ref<@Syn, mut>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn, mut> -> f32
      ent.set %s "w", %w : !ent.ref<@Syn, mut>, f32
    }
  }
}

// A frame starts by sorting edges the host connected.
// CHECK-LABEL: func.func @frame(
// CHECK-NEXT:   call @ent_sort_Syn(
// The sort: only when unclean; offsets by source, then a stable scatter.
// CHECK-LABEL: func.func private @ent_sort_Syn(
// CHECK:        %[[CLEAN:.*]] = memref.load
// CHECK:        scf.if
// CHECK:          scf.for
// CHECK:          scf.for
// CHECK:          scf.for
// CHECK:          scf.for
// CHECK:          scf.for
// CHECK:          scf.for
// CHECK:          %[[ONE:.*]] = arith.constant 1 : i64
// CHECK-NEXT:     memref.store %[[ONE]]
ent.schedule @frame() {
  ent.run @push()
  ent.run @learn()
}

// -----

// Generational ids: a slot may have been reused since the edges were
// sorted, so an edge counts only if its own end is the visited entity.
ent.component @N (v: f32) capacity 100
ent.archetype @A (@N) capacity 100
ent.relation @Syn (w: f32) capacity 1000

// CHECK-LABEL: func.func private @pull(
// CHECK:        %[[ID:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<100xi32>
// CHECK:        scf.for %[[P:.*]] =
// CHECK-NEXT:     %[[POS:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     %[[EDGE:.*]] = arith.index_cast %[[POS]]
// CHECK-NEXT:     %[[OWN:.*]] = memref.load %{{.*}}[%[[EDGE]]] : memref<1000xi32>
// CHECK-NEXT:     %[[MINE:.*]] = arith.cmpi eq, %[[OWN]], %[[ID]] : i32
// CHECK-NEXT:     scf.if %[[MINE]] {
ent.system @pull() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn>, %p: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      ent.set %n "v", %w : !ent.ref<@N, mut>, f32
    }
  }
}

// A query that disconnects or connects sorts when it ends.
// CHECK-LABEL: func.func private @prune(
// CHECK:        scf.for
// CHECK:          arith.constant 1 : i8
// CHECK:          arith.constant 0 : i64
// CHECK:      call @ent_sort_Syn(
// ACCESS: remark: reads entities, Syn.edges, A.id, A.count; writes Syn.edges, Syn.w, entities, A.count, A.id, A.N.v
ent.system @prune() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn out (%s: !ent.ref<@Syn>, %t: !ent.entity) {
      ent.disconnect
    }
    ent.despawn
  }
}
