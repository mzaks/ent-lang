// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s
// RUN: ecs-opt %s "--ecs-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PAR
// RUN: ecs-opt %s --ecs-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @P (x: f32)
ecs.component @L (t: f32)
ecs.component @S (s: f32)
ecs.archetype @Bullet (@P, @L, optional @S) capacity 100

// Layout: the count at 0, the pending counter for @Bullet at 8, columns
// P.x at 1152, L.t at 2688, S.s at 4224, S? at 5760, and the pending list
// after them at 5888 + 1088 = 6976.

// A despawn lists the row; after the loop, the listed rows are removed
// last first by moving the last row into them.
// CHECK-LABEL: func.func private @age(
// CHECK:      %[[COUNTS:.*]] = memref.view {{.*}} to memref<1xi64>
// CHECK:      arith.constant 8 : index
// CHECK-NEXT: %[[PENDING_N:.*]] = memref.view {{.*}} to memref<1xi64>
// CHECK:      arith.constant 6976 : index
// CHECK-NEXT: %[[PENDING:.*]] = memref.view {{.*}} to memref<100xi32>
// CHECK:      scf.for %[[I:.*]] =
// CHECK:        scf.if
// CHECK-NEXT:     %[[C0:.*]] = arith.constant 0 : index
// CHECK-NEXT:     %[[K:.*]] = memref.load %[[PENDING_N]][%[[C0]]]
// CHECK-NEXT:     %[[KI:.*]] = arith.index_cast %[[K]] : i64 to index
// CHECK-NEXT:     %[[ROW:.*]] = arith.index_cast %[[I]] : index to i32
// CHECK-NEXT:     memref.store %[[ROW]], %[[PENDING]][%[[KI]]]
// CHECK:          memref.store %{{.*}}, %[[PENDING_N]][%[[C0]]]
// CHECK:      %[[N:.*]] = memref.load %[[PENDING_N]]
// CHECK-NEXT: %[[NI:.*]] = arith.index_cast %[[N]]
// CHECK-NEXT: scf.for %[[J:.*]] = %{{.*}} to %[[NI]]
// CHECK:        %[[REV:.*]] = arith.subi %{{.*}}, %[[J]] : index
// CHECK-NEXT:   %[[R:.*]] = memref.load %[[PENDING]][%[[REV]]]
// CHECK:        %[[LAST:.*]] = arith.subi
// CHECK-NEXT:   %[[MOVES:.*]] = arith.cmpi ne, %{{.*}}, %[[LAST]] : index
// CHECK-NEXT:   scf.if %[[MOVES]] {
// Every column moves, the presence of the optional component included.
// CHECK-COUNT-4: memref.load
// CHECK:        }
// CHECK:        memref.store %{{.*}}, %[[COUNTS]]
// CHECK:      }
// CHECK:      memref.store %{{.*}}, %[[PENDING_N]]
// CHECK-NEXT: return
ecs.system @age(%dt: f32) writes [@L, @Bullet] {
  ecs.query (%l: !ecs.ref<@L, mut>) {
    %t = ecs.get %l "t" : !ecs.ref<@L, mut> -> f32
    %n = arith.subf %t, %dt : f32
    ecs.set %l "t", %n : !ecs.ref<@L, mut>, f32
    %zero = arith.constant 0.0 : f32
    %dead = arith.cmpf ole, %n, %zero : f32
    scf.if %dead {
      ecs.despawn
    }
  }
}

// A spawn checks the capacity, writes the non-optional fields and an
// absent presence into row `count`, and bumps the count.
// CHECK-LABEL: func.func private @fire(
// CHECK-SAME: %[[X:[^:]*]]: f32
// CHECK:      %[[ROWI:.*]] = arith.index_cast %{{.*}} : i64 to index
// CHECK-NEXT: %[[CAP:.*]] = arith.constant 100 : index
// CHECK-NEXT: %[[FITS:.*]] = arith.cmpi ult, %[[ROWI]], %[[CAP]] : index
// CHECK-NEXT: cf.assert %[[FITS]], "ecs.spawn exceeds the capacity of @Bullet"
// CHECK-NEXT: memref.store %[[X]], %{{.*}}[%[[ROWI]]] : memref<100xf32>
// CHECK-NEXT: memref.store %{{.*}}, %{{.*}}[%[[ROWI]]] : memref<100xf32>
// CHECK-NEXT: %[[ABSENT:.*]] = arith.constant 0 : i8
// CHECK-NEXT: memref.store %[[ABSENT]], %{{.*}}[%[[ROWI]]] : memref<100xi8>
// CHECK:      %[[NEXT:.*]] = arith.addi %[[ROWI]]
// CHECK:      arith.index_cast %[[NEXT]] : index to i64
// CHECK-NEXT: memref.store
ecs.system @fire(%x: f32) writes [@Bullet] {
  %t = arith.constant 1.0 : f32
  ecs.spawn @Bullet(%x, %t) : f32, f32
}

ecs.system @drift(%d: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
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
ecs.schedule @frame(%dt: f32, %x: f32) {
  ecs.run @drift(%dt) : f32
  ecs.run @age(%dt) : f32
  ecs.run @fire(%x) : f32
  ecs.run @drift(%dt) : f32
}
