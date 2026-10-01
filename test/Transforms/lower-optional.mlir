// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @V (dx: f32)
ent.component @S (t: f32)
ent.component @N (n: i32)
ent.archetype @C (@V, optional @S, optional @N) capacity 100

// Columns: V.dx at 1152, S.t at 2688, S? at 4224, N.n at 5440, N? at 6976.

// A query binding an optional component runs for every entity and masks
// its stores with the presence (branch-free).
// CHECK-LABEL: func.func private @status(
// CHECK:      arith.constant 4224 : index
// CHECK:      scf.for %[[I:.*]] =
// CHECK-NEXT:   %[[P:.*]] = memref.load %[[PRESENT:.*]][%[[I]]] : memref<100xi8>
// CHECK-NEXT:   %[[ZERO:.*]] = arith.constant 0 : i8
// CHECK-NEXT:   %[[MASK:.*]] = arith.cmpi ne, %[[P]], %[[ZERO]] : i8
// CHECK:        %[[NEW:.*]] = arith.mulf
// CHECK-NEXT:   %[[OLD:.*]] = memref.load %[[DX:.*]][%[[I]]] : memref<100xf32>
// CHECK-NEXT:   %[[SEL:.*]] = arith.select %[[MASK]], %[[NEW]], %[[OLD]] : f32
// CHECK-NEXT:   memref.store %[[SEL]], %[[DX]][%[[I]]] : memref<100xf32>
// Removing the component stores a masked 0 into the presence.
// CHECK:        scf.if
// CHECK-NEXT:     %[[C0:.*]] = arith.constant 0 : i8
// CHECK-NEXT:     %[[OLDP:.*]] = memref.load %[[PRESENT]][%[[I]]]
// CHECK-NEXT:     %[[SELP:.*]] = arith.select %[[MASK]], %[[C0]], %[[OLDP]] : i8
// CHECK-NEXT:     memref.store %[[SELP]], %[[PRESENT]][%[[I]]]
ent.system @status(%dt: f32) writes [@V, @S] {
  ent.query (%v: !ent.ref<@V, mut>, %s: !ent.ref<@S, mut>) {
    %dx = ent.get %v "dx" : !ent.ref<@V, mut> -> f32
    %slow = arith.constant 0.99 : f32
    %n = arith.mulf %dx, %slow : f32
    ent.set %v "dx", %n : !ent.ref<@V, mut>, f32
    %t = ent.get %s "t" : !ent.ref<@S, mut> -> f32
    %left = arith.subf %t, %dt : f32
    ent.set %s "t", %left : !ent.ref<@S, mut>, f32
    %zero = arith.constant 0.0 : f32
    %done = arith.cmpf ole, %left, %zero : f32
    scf.if %done {
      ent.remove @S
    }
  }
}

// Adding from a query that does not bind the component: unmasked stores of
// the fields and of presence 1.
// CHECK-LABEL: func.func private @stun(
// CHECK:      scf.for %[[I:.*]] =
// CHECK-NEXT:   memref.store %{{.*}}, %{{.*}}[%[[I]]] : memref<100xf32>
// CHECK-NEXT:   %[[ONE:.*]] = arith.constant 1 : i8
// CHECK-NEXT:   memref.store %[[ONE]], %{{.*}}[%[[I]]] : memref<100xi8>
// CHECK-NEXT: }
ent.system @stun(%t: f32) reads [@V] writes [@S] {
  ent.query (%v: !ent.ref<@V>) {
    ent.add @S(%t) : f32
  }
}

// Dividing by a stale value may be undefined for an absent entity, so this
// body runs behind a branch on both presences, unmasked. (Dividing by a
// non-zero constant would be speculatable and stay branch-free.)
// CHECK-LABEL: func.func private @divide(
// CHECK:      scf.for %[[I:.*]] =
// CHECK:        %[[M1:.*]] = arith.cmpi ne
// CHECK:        %[[M2:.*]] = arith.cmpi ne
// CHECK-NEXT:   %[[M:.*]] = arith.andi %[[M1]], %[[M2]] : i1
// CHECK-NEXT:   scf.if %[[M]] {
// CHECK:          arith.divsi
// CHECK-NOT:      arith.select
// CHECK:          memref.store
// CHECK-NEXT:   }
ent.system @divide() reads [@S] writes [@N] {
  ent.query (%n: !ent.ref<@N, mut>, %s: !ent.ref<@S>) {
    %x = ent.get %n "n" : !ent.ref<@N, mut> -> i32
    %hundred = arith.constant 100 : i32
    %h = arith.divsi %hundred, %x : i32
    ent.set %n "n", %h : !ent.ref<@N, mut>, i32
  }
}

// Fused, each query keeps its own mask: stun's add is unmasked, status'
// stores are masked by the presence it reads after the add.
// FUSED-LABEL: func.func @frame(
// FUSED:       scf.for %[[I:.*]] =
// FUSED-NEXT:    memref.store %{{.*}}, %{{.*}}[%[[I]]] : memref<100xf32>
// FUSED-NEXT:    arith.constant 1 : i8
// FUSED-NEXT:    memref.store
// FUSED-NEXT:    %[[P:.*]] = memref.load %{{.*}}[%[[I]]] : memref<100xi8>
// FUSED:         arith.select
// FUSED-NOT:   scf.for
// FUSED:       return
ent.schedule @frame(%t: f32, %dt: f32) {
  ent.run @stun(%t) : f32
  ent.run @status(%dt) : f32
}
