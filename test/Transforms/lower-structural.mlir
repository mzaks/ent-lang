// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @P (x: f32)
ent.component @L (t: f32)
ent.component @S (s: f32)
ent.archetype @Bullet (@P, @L, optional @S) capacity 100

// Layout: the count at 0, the pending counter for @Bullet at 8, the entity
// table's next-slot counter and free-list head at 16 and 24; columns P.x
// at 1152, L.t at 2688, S.s at 4224, S? at 5760, the id column at 6976,
// the pending list at 8512; then the entity table: generations at 10048
// and packed locations (archetype << rowBits | row) at 11584. Entities are
// despawned, so ids are generational: 100 slots need 7 bits, which leaves
// 25 generation bits in a 32-bit id.

// A despawn lists the row; if the last listed row is already this entity,
// it replaces that entry (the last structural change wins). After the
// loop, the listed rows are handled last first: the entity's id is freed
// (its generation bumped, its slot pushed on the free list, which runs
// through the free slots' locations), and the archetype's last row, id
// included, moves into the hole, with the moved entity's location updated.
// CHECK-LABEL: func.func private @age(
// CHECK:      %[[COUNTS:.*]] = memref.view {{.*}} to memref<1xi64>
// CHECK:      arith.constant 8 : index
// CHECK-NEXT: %[[PENDING_N:.*]] = memref.view {{.*}} to memref<1xi64>
// CHECK:      arith.constant 8512 : index
// CHECK-NEXT: %[[PENDING:.*]] = memref.view {{.*}} to memref<100xi32>
// CHECK:      arith.constant 6976 : index
// CHECK-NEXT: %[[IDS:.*]] = memref.view {{.*}} to memref<100xi32>
// CHECK:      arith.constant 10048 : index
// CHECK-NEXT: %[[GENERATION:.*]] = memref.view {{.*}} to memref<100xi32>
// CHECK:      arith.constant 24 : index
// CHECK-NEXT: %[[FREE_HEAD:.*]] = memref.view {{.*}} to memref<1xi64>
// CHECK:      arith.constant 11584 : index
// CHECK-NEXT: %[[LOCATION:.*]] = memref.view {{.*}} to memref<100xi32>
// CHECK:      scf.for %[[I:.*]] =
// CHECK:        scf.if
// CHECK:          %[[K:.*]] = memref.load %[[PENDING_N]]
// CHECK:          %[[ROW:.*]] = arith.index_cast %[[I]] : index to i32
// CHECK:          %[[SAME:.*]] = scf.if %{{.*}} -> (i1) {
// CHECK:            %[[PREV:.*]] = memref.load %[[PENDING]]
// CHECK-NEXT:       arith.cmpi eq, %[[PREV]], %[[ROW]] : i32
// CHECK:          %[[SLOT:.*]] = arith.select %[[SAME]], %{{.*}}, %[[K]] : i64
// CHECK:          memref.store %[[ROW]], %[[PENDING]]
// CHECK:          memref.store %{{.*}}, %[[PENDING_N]]
// CHECK:      scf.for %[[J:.*]] =
// CHECK:        %[[R:.*]] = memref.load %[[PENDING]]
// CHECK:        %[[RI:.*]] = arith.index_cast %[[R]] : i32 to index
// CHECK:        %[[ID:.*]] = memref.load %[[IDS]][%[[RI]]] : memref<100xi32>
// CHECK:        %[[IDSLOT:.*]] = arith.andi %[[ID]], %{{.*}} : i32
// CHECK-NEXT:   %[[IDX:.*]] = arith.index_castui %[[IDSLOT]] : i32 to index
// CHECK-NEXT:   %[[GEN:.*]] = memref.load %[[GENERATION]][%[[IDX]]]
// CHECK:        %[[GEN1:.*]] = arith.addi %[[GEN]]
// The generation wraps at its 25 bits, not at its 32-bit storage.
// CHECK-NEXT:   %[[MASK:.*]] = arith.constant 33554431 : i32
// CHECK-NEXT:   %[[WRAPPED:.*]] = arith.andi %[[GEN1]], %[[MASK]] : i32
// CHECK-NEXT:   memref.store %[[WRAPPED]], %[[GENERATION]][%[[IDX]]]
// CHECK:        %[[HEAD:.*]] = memref.load %[[FREE_HEAD]]
// CHECK-NEXT:   %[[NEXT:.*]] = arith.trunci %[[HEAD]] : i64 to i32
// CHECK-NEXT:   memref.store %[[NEXT]], %[[LOCATION]][%[[IDX]]]
// CHECK:        memref.store %{{.*}}, %[[FREE_HEAD]]
// CHECK:        %[[LAST:.*]] = arith.subi
// CHECK-NEXT:   %[[MOVES:.*]] = arith.cmpi ne, %[[RI]], %[[LAST]] : index
// CHECK-NEXT:   scf.if %[[MOVES]] {
// CHECK-COUNT-4: memref.load
// CHECK:          %[[MOVED:.*]] = memref.load %[[IDS]][%[[LAST]]]
// CHECK-NEXT:     memref.store %[[MOVED]], %[[IDS]][%[[RI]]]
// CHECK:          memref.store %{{.*}}, %[[LOCATION]]
// CHECK:        }
// CHECK:        memref.store %{{.*}}, %[[COUNTS]]
// CHECK:      }
// CHECK:      memref.store %{{.*}}, %[[PENDING_N]]
// CHECK-NEXT: return
ent.system @age(%dt: f32) writes [@L, @Bullet] {
  ent.query (%l: !ent.ref<@L, mut>) {
    %t = ent.get %l "t" : !ent.ref<@L, mut> -> f32
    %n = arith.subf %t, %dt : f32
    ent.set %l "t", %n : !ent.ref<@L, mut>, f32
    %zero = arith.constant 0.0 : f32
    %dead = arith.cmpf ole, %n, %zero : f32
    scf.if %dead {
      ent.despawn
    }
  }
}

// A spawn checks the capacity, writes the non-optional fields and an
// absent presence into row `count`, allocates an id (the head of the free
// list if there is one, otherwise the next unused slot with generation 0),
// records the id's packed location and the row's id, and bumps the count.
// CHECK-LABEL: func.func private @fire(
// CHECK-SAME: %[[X:[^:]*]]: f32
// CHECK:      %[[ROWI:.*]] = arith.index_cast %{{.*}} : i64 to index
// CHECK-NEXT: %[[CAP:.*]] = arith.constant 100 : index
// CHECK-NEXT: %[[FITS:.*]] = arith.cmpi ult, %[[ROWI]], %[[CAP]] : index
// CHECK-NEXT: cf.assert %[[FITS]], "ent.spawn exceeds the capacity of @Bullet"
// CHECK-NEXT: memref.store %[[X]], %{{.*}}[%[[ROWI]]] : memref<100xf32>
// CHECK-NEXT: memref.store %{{.*}}, %{{.*}}[%[[ROWI]]] : memref<100xf32>
// CHECK-NEXT: %[[ABSENT:.*]] = arith.constant 0 : i8
// CHECK-NEXT: memref.store %[[ABSENT]], %{{.*}}[%[[ROWI]]] : memref<100xi8>
// CHECK:      %[[SLOT:.*]] = scf.if %{{.*}} -> (index) {
// The free list's next link is in the slot's location.
// CHECK:        %[[LINK:.*]] = memref.load %[[LOCATION:.*]][%{{.*}}] : memref<100xi32>
// CHECK:      } else {
// CHECK:      }
// CHECK:      memref.store %{{.*}}, %[[LOCATION]][%[[SLOT]]] : memref<100xi32>
// CHECK:      %[[HIGH:.*]] = arith.shli
// CHECK:      %[[ID:.*]] = arith.ori %[[HIGH]], %{{.*}} : i32
// CHECK-NEXT: memref.store %[[ID]], %{{.*}}[%[[ROWI]]] : memref<100xi32>
// CHECK:      %[[NEXT:.*]] = arith.addi %[[ROWI]]
// CHECK:      arith.index_cast %[[NEXT]] : index to i64
// CHECK-NEXT: memref.store
ent.system @fire(%x: f32) writes [@Bullet] {
  %t = arith.constant 1.0 : f32
  ent.spawn @Bullet(%x, %t) : f32, f32
}

ent.system @drift(%d: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}

// Appending to the pending list from several threads would race: a query
// that despawns stays sequential even with parallel loops forced on.
// PAR-LABEL: func.func private @age(
// PAR-NOT:   scf.parallel
// PAR:       scf.for
// PAR-LABEL: func.func private @drift(
// PAR:       scf.parallel

// A system with structural changes ends a fused sequence and stays a call.
// FUSED-LABEL: func.func @frame(
// FUSED:       scf.for
// FUSED:       call @age(
// FUSED:       call @fire(
// FUSED:       scf.for
// FUSED:       return
ent.schedule @frame(%dt: f32, %x: f32) {
  ent.run @drift(%dt) : f32
  ent.run @age(%dt) : f32
  ent.run @fire(%x) : f32
  ent.run @drift(%dt) : f32
}
