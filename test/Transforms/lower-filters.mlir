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
ent.resource @Paused (value: i1)
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

// has answers as of the query's start: the presence is read before the add
// stores it. In A, F is never held: false.
// CHECK-LABEL: func.func private @toggle(
// CHECK:      scf.for %[[I:.*]] =
// CHECK-NEXT:   %[[P:.*]] = memref.load %[[S:.*]][%[[I]]] : memref<100xi8>
// CHECK-NEXT:   %{{.*}} = arith.constant 0 : i8
// CHECK-NEXT:   %[[HAS:.*]] = arith.cmpi ne, %[[P]], %{{.*}} : i8
// CHECK-NEXT:   %[[ONE:.*]] = arith.constant 1 : i8
// CHECK-NEXT:   memref.store %[[ONE]], %[[S]][%[[I]]] : memref<100xi8>
// CHECK-NEXT:   %[[NO:.*]] = arith.constant false
// CHECK-NEXT:   %[[X:.*]] = memref.load
// CHECK-NEXT:   %[[Y:.*]] = arith.select %[[HAS]], %[[X]], %[[X]] : f32
// CHECK-NEXT:   arith.select %[[NO]], %[[Y]], %[[Y]] : f32
// The test of F in C reuses the mask's load.
// CHECK:      scf.for %[[J:.*]] =
// CHECK-NEXT:   memref.load %{{.*}}[%[[J]]] : memref<100xi8>
// CHECK-NEXT:   arith.constant 0 : i8
// CHECK-NEXT:   %[[FC:.*]] = arith.cmpi ne
// CHECK:        arith.select %[[FC]]
// ACCESS: remark: reads A.S?, C.S?, C.F?, A.P.x, C.P.x, A.count, C.count; writes A.S?, C.S?, A.P.x, C.P.x
ent.system @toggle() {
  ent.query (%p: !ent.ref<@P, mut>) without [@F] {
    ent.add @S()
    %s = ent.has @S
    %f = ent.has @F
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %y = arith.select %s, %x, %x : f32
    %z = arith.select %f, %y, %y : f32
    ent.set %p "x", %z : !ent.ref<@P, mut>, f32
  }
}

// The schedule's condition guards its whole body; a run's guards its call.
// CHECK-LABEL: func.func @frame(
// CHECK:        %[[PAUSED:.*]] = memref.load
// CHECK:        %[[GO:.*]] = arith.xori %[[PAUSED]]
// CHECK-NEXT:   scf.if %[[GO]] {
// CHECK-NEXT:     func.call @strike(
// CHECK-NEXT:     scf.execute_region {
// CHECK:            %[[C:.*]] = memref.load
// CHECK-NEXT:       scf.if %[[C]] {
// CHECK-NEXT:         func.call @elements(
// CHECK-NEXT:       }
// CHECK-NEXT:       scf.yield
// CHECK-NEXT:     }
// CHECK-NEXT:     func.call @toggle(
// CHECK-NEXT:   }
// CHECK-NEXT:   return

// A run under a condition is not fused with its neighbours.
// FUSED-LABEL: func.func @frame(
// FUSED:         scf.if
// FUSED:           scf.for
// FUSED:           scf.execute_region {
// FUSED:             func.call @elements(
// FUSED:           scf.for
// FUSED-NOT:     func.call @strike
// FUSED-NOT:     func.call @toggle
ent.schedule @frame() if {
  %paused = ent.read @Paused "value" : i1
  %true = arith.constant true
  %run = arith.xori %paused, %true : i1
  ent.yield %run : i1
} {
  ent.run @strike()
  ent.run @elements() if {
    %paused = ent.read @Paused "value" : i1
    ent.yield %paused : i1
  }
  ent.run @toggle()
}
