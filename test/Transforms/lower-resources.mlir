// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @P (x: f32)
ent.resource @Clock (dt: f32, frame: i64)
ent.archetype @Many (@P) capacity 1000
ent.archetype @Player (@P) capacity 1

// World layout: 2 counts, then Clock on its own cache line (dt at 64,
// frame at 72), then columns: Many.P.x at 128 + 1088 = 1216, Player.P.x at
// 5248 + 1088 = 6336.

// A resource read inside a query is loaded once, before the loop; a system
// that writes the resource before the query is seen by it, since the loop
// comes after the write.
// CHECK-LABEL: func.func private @advance(
// CHECK-SAME: %[[W:[^:]*]]: memref<16384xi8>)
// CHECK:      arith.constant 72 : index
// CHECK-NEXT: %[[FRAME:.*]] = memref.view %[[W]]{{.*}} to memref<1xi64>
// CHECK:      %[[F:.*]] = memref.load %[[FRAME]][%{{.*}}] : memref<1xi64>
// CHECK:      %[[F1:.*]] = arith.addi %[[F]]
// CHECK:      memref.store %[[F1]], %[[FRAME]][%{{.*}}] : memref<1xi64>
// CHECK:      %[[DT:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<1xf32>
// CHECK-NEXT: scf.for
// CHECK-NOT:    memref<1xf32>
// CHECK:        arith.addf %{{.*}}, %[[DT]]
// CHECK:      }
// The single player gets a guard instead of a loop, at entity 0.
// CHECK:      %[[ANY:.*]] = arith.cmpi sgt, %{{.*}}, %[[ZERO:.*]] : index
// CHECK:      %[[DT2:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<1xf32>
// CHECK-NEXT: scf.if %[[ANY]] {
// CHECK-NEXT:   %[[X:.*]] = memref.load %{{.*}}[%[[ZERO]]] : memref<1xf32>
// CHECK-NEXT:   %[[X1:.*]] = arith.addf %[[X]], %[[DT2]]
// CHECK-NEXT:   memref.store %[[X1]], %{{.*}}[%[[ZERO]]] : memref<1xf32>
// CHECK-NEXT: }
ent.system @advance() writes [@P, @Clock] {
  %f = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %f, %one : i64
  ent.write @Clock "frame", %next : i64
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %dt = ent.read @Clock "dt" : f32
    %n = arith.addf %x, %dt : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}

ent.system @drift() reads [@Clock] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %dt = ent.read @Clock "dt" : f32
    %n = arith.subf %x, %dt : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}

// A system that writes a resource before its query is fused where no
// query before it reads what it writes: the drifts read dt, advance writes
// frame. Its write moves ahead of the loop, and each entity drifts,
// advances and drifts in one pass.
// FUSED-LABEL: func.func @frame(
// FUSED-NOT:   call @advance(
// FUSED:       arith.addi
// FUSED:       memref.store {{.*}} : memref<1xi64>
// FUSED:       scf.for
// FUSED:         arith.subf
// FUSED:         arith.addf
// FUSED:         arith.subf
// FUSED:       scf.if
// FUSED:       return
ent.schedule @frame() {
  ent.run @drift()
  ent.run @advance()
  ent.run @drift()
}

// One that reads what a later system writes, before its loop, ends the
// sequence there: `tick` reads frame, which `advance` writes, so advance
// starts the next sequence, after tick's loop.
ent.system @tick() reads [@Clock] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %f = ent.read @Clock "frame" : i64
    %ff = arith.sitofp %f : i64 to f32
    %n = arith.mulf %x, %ff : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
// FUSED-LABEL: func.func @counted(
// FUSED:       scf.for
// FUSED:         arith.mulf
// FUSED:       arith.addi
// FUSED:       memref.store {{.*}} : memref<1xi64>
// FUSED:       scf.for
// FUSED:         arith.addf
// FUSED:       return
ent.schedule @counted() {
  ent.run @tick()
  ent.run @advance()
}
