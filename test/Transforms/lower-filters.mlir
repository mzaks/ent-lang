// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED
// RUN: ent-opt %s --ent-print-access 2>&1 | FileCheck %s --check-prefix=ACCESS

// Filters decide per archetype where they can and test the presence per
// entity where the component is optional.
ent.component @P (x: f32)
ent.component @S ()
ent.component @F ()
ent.component @I ()
ent.archetype @A (@P, optional @S) capacity 100
ent.archetype @B (@P, @F) capacity 100
ent.archetype @C (@P, optional @F, optional @I, optional @S) capacity 100

// Without S: masked by "S absent" where S is optional (A, C), unmasked
// where it is never held (B).
// CHECK-LABEL: func.func private @strike(
// CHECK:      scf.for %[[I:.*]] =
// CHECK-NEXT:   %[[P:.*]] = memref.load %{{.*}}[%[[I]]] : memref<100xi8>
// CHECK-NEXT:   %{{.*}} = arith.constant 0 : i8
// CHECK-NEXT:   %[[HAS:.*]] = arith.cmpi ne, %[[P]], %{{.*}} : i8
// CHECK-NEXT:   %{{.*}} = arith.constant true
// CHECK-NEXT:   %[[MASK:.*]] = arith.xori %[[HAS]], %{{.*}} : i1
// CHECK:        arith.select %[[MASK]]
// CHECK:      scf.for %[[J:.*]] =
// CHECK-NEXT:   memref.load %{{.*}}[%[[J]]] : memref<100xf32>
// CHECK-NEXT:   arith.constant
// CHECK-NEXT:   arith.subf
// CHECK-NEXT:   memref.store
// CHECK-NEXT: }
// CHECK:      scf.for
// CHECK:        arith.xori
// CHECK:        arith.select
// ACCESS: remark: reads A.P.x, B.P.x, C.P.x, A.count, B.count, C.count, A.S?, C.S?; writes A.P.x, B.P.x, C.P.x
ent.system @strike() {
  ent.query (%p: !ent.ref<@P, mut>) without [@S] {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %one = arith.constant 1.0 : f32
    %y = arith.subf %x, %one : f32
    ent.set %p "x", %y : !ent.ref<@P, mut>, f32
  }
}

// Any of F, I: A holds neither and is skipped; B always holds F; in C one
// of the two must be present.
// CHECK-LABEL: func.func private @elements(
// CHECK:      scf.for %[[I:.*]] =
// CHECK-NEXT:   memref.load %{{.*}}[%[[I]]] : memref<100xf32>
// CHECK-NEXT:   arith.constant
// CHECK-NEXT:   arith.addf
// CHECK-NEXT:   memref.store
// CHECK-NEXT: }
// CHECK:      scf.for %[[J:.*]] =
// CHECK:        %[[F:.*]] = arith.cmpi ne
// CHECK:        %[[IC:.*]] = arith.cmpi ne
// CHECK-NEXT:   %[[ANY:.*]] = arith.ori %[[F]], %[[IC]] : i1
// CHECK:        arith.select %[[ANY]]
// CHECK-NOT:  scf.for
// CHECK:      return
// ACCESS: remark: reads B.P.x, C.P.x, B.count, C.count, C.F?, C.I?; writes B.P.x, C.P.x
ent.system @elements() {
  ent.query (%p: !ent.ref<@P, mut>) any [@F, @I] {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %one = arith.constant 1.0 : f32
    %y = arith.addf %x, %one : f32
    ent.set %p "x", %y : !ent.ref<@P, mut>, f32
  }
}

// Both are fused.
// FUSED-LABEL: func.func @frame(
// FUSED-NOT:     func.call
// FUSED:         return
ent.schedule @frame() {
  ent.run @strike()
  ent.run @elements()
}
