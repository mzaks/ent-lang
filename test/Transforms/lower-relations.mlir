// RUN: ent-opt %s --split-input-file --ent-lower-to-loops=direct-applies=0 \
// RUN:   | FileCheck %s
// RUN: ent-opt %s --split-input-file --ent-lower-to-loops \
// RUN:   | FileCheck %s --check-prefix=DIRECT
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
// In a loop that never runs in parallel the apply is combined as the edges
// are visited (rows, then edges: the combine's order), without buffers.
// DIRECT-LABEL: func.func private @push(
// DIRECT:      scf.for %[[ROW:.*]] =
// DIRECT-NOT:    memref<100xi32>
// DIRECT:        scf.for %[[P:.*]] =
// DIRECT:          %[[T:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// DIRECT-NEXT:     %[[W:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xf32>
// DIRECT-NEXT:     %[[X:.*]] = arith.mulf %{{.*}}, %[[W]] : f32
// DIRECT:            %[[OLD:.*]] = memref.load %[[IN:.*]][%[[AT:.*]]] : memref<100xf32>
// DIRECT-NEXT:       %[[NEW:.*]] = arith.addf %[[OLD]], %[[X]] : f32
// DIRECT-NEXT:       memref.store %[[NEW]], %[[IN]][%[[AT]]] : memref<100xf32>
// DIRECT-LABEL: func.func private @learn(
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

// Visited both ways here, so `in` reads through the index by target. The
// entity's own field the loop sets is carried as a value (through the
// ownership check's `if`) and stored once after the loop, if it was set.
// CHECK-LABEL: func.func private @pull(
// CHECK:        %[[ID:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<100xi32>
// CHECK:        %[[V0:.*]] = memref.load %[[V:.*]][%[[ROW:.*]]] : memref<100xf32>
// CHECK-NEXT:   %[[NO:.*]] = arith.constant false
// CHECK-NEXT:   %[[R:.*]]:2 = scf.for %[[P:.*]] = {{.*}} iter_args(%[[CUR:.*]] = %[[V0]], %[[SET:.*]] = %[[NO]]) -> (f32, i1) {
// CHECK-NEXT:     %[[POS:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     %[[EDGE:.*]] = arith.index_cast %[[POS]]
// CHECK-NEXT:     %[[OWN:.*]] = memref.load %{{.*}}[%[[EDGE]]] : memref<1000xi32>
// CHECK-NEXT:     %[[MINE:.*]] = arith.cmpi eq, %[[OWN]], %[[ID]] : i32
// CHECK-NEXT:     %[[B:.*]]:2 = scf.if %[[MINE]] -> (f32, i1) {
// CHECK:            scf.yield %{{.*}}, %{{.*}} : f32, i1
// CHECK-NEXT:     } else {
// CHECK-NEXT:       scf.yield %[[CUR]], %[[SET]] : f32, i1
// CHECK-NEXT:     }
// CHECK-NEXT:     scf.yield %[[B]]#0, %[[B]]#1 : f32, i1
// CHECK-NEXT:   }
// CHECK-NEXT:   scf.if %[[R]]#1 {
// CHECK-NEXT:     memref.store %[[R]]#0, %[[V]][%[[ROW]]] : memref<100xf32>
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

// -----

// Visited only by incoming edges: the table is sorted by target, so the
// loop reads sources and weights in order, without the index.
ent.component @N (v: f32, input: f32) capacity 100
ent.archetype @A (@N) capacity 100
ent.relation @Syn (w: f32) capacity 1000

// CHECK-LABEL: func.func private @gather(
// CHECK:        %[[B:.*]] = memref.load %[[OFF:.*]][%{{.*}}] : memref<129xi32>
// CHECK:        scf.for %[[P:.*]] = {{.*}} iter_args(
// CHECK-NEXT:     %[[SRC:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// CHECK-NEXT:     %[[W:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xf32>
// CHECK-NOT:      memref.store
// CHECK:          scf.yield
// The sort: by source into the scratch, then by target back.
// CHECK-LABEL: func.func private @ent_sort_Syn(
ent.system @gather() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn>, %p: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      %i = ent.get %n "input" : !ent.ref<@N, mut> -> f32
      %j = arith.addf %i, %w : f32
      ent.set %n "input", %j : !ent.ref<@N, mut>, f32
    }
  }
}

ent.schedule @frame() {
  ent.run @gather()
}

// -----

// Typed ends that nothing takes away: a lookup through the edge's other end
// reads the field directly, without checking the id.
ent.component @N (v: f32, input: f32) capacity 100
ent.archetype @A (@N) capacity 100
ent.relation @Syn (w: f32) from @N to @N capacity 1000

// CHECK-LABEL: func.func private @trusted(
// CHECK:        scf.for %[[P:.*]] = {{.*}} iter_args(
// CHECK-NEXT:     %[[SRC:.*]] = memref.load %{{.*}}[%[[P]]] : memref<1000xi32>
// CHECK:          %[[ROW:.*]] = arith.index_castui %{{.*}} : i32 to index
// CHECK-NEXT:     %[[V:.*]] = memref.load %{{.*}}[%[[ROW]]] : memref<100xf32>
// CHECK-NEXT:     %{{.*}} = arith.constant true
ent.system @trusted() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn>, %p: !ent.entity) {
      %v, %found = ent.lookup %p @N "v" : f32
      %i = ent.get %n "input" : !ent.ref<@N, mut> -> f32
      %j = arith.addf %i, %v : f32
      ent.set %n "input", %j : !ent.ref<@N, mut>, f32
    }
  }
}

// Connecting checks both ends.
// CHECK-LABEL: func.func private @wire(
// CHECK:        cf.assert %{{.*}}, "ent.connect: the source of an edge of @Syn does not have @N"
// CHECK:        cf.assert %{{.*}}, "ent.connect: the target of an edge of @Syn does not have @N"
ent.system @wire(%a: !ent.entity, %b: !ent.entity) {
  %w = arith.constant 1.0 : f32
  ent.connect @Syn %a, %b (%w) : f32
}

ent.schedule @frame() {
  ent.run @trusted()
}

// -----

// The same where a system despawns entities with @N: their edges may
// outlive them until the next sort, so the lookup checks the id.
ent.component @N (v: f32, input: f32) capacity 100
ent.archetype @A (@N) capacity 100
ent.relation @Syn (w: f32) from @N to @N capacity 1000

// CHECK-LABEL: func.func private @checked(
// CHECK:        scf.for %{{.*}} = {{.*}} iter_args(
// CHECK:          memref.load %{{.*}} : memref<1xi64>
// CHECK:          arith.cmpi ult
ent.system @checked() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn>, %p: !ent.entity) {
      %v, %found = ent.lookup %p @N "v" : f32
      %i = ent.get %n "input" : !ent.ref<@N, mut> -> f32
      %j = arith.addf %i, %v : f32
      ent.set %n "input", %j : !ent.ref<@N, mut>, f32
    }
  }
}

ent.system @cull() {
  ent.query (%n: !ent.ref<@N>) {
    ent.despawn
  }
}
