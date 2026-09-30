// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s
// RUN: ecs-opt %s --ecs-lower-to-loops=fuse-systems=1 --symbol-dce \
// RUN:   | FileCheck %s --check-prefix=FUSED

ecs.component @P (x: f32)
ecs.resource @Clock (dt: f32, frame: i64)
ecs.archetype @Many (@P) capacity 1000
ecs.archetype @Player (@P) capacity 1

// World layout: 2 counts, then Clock on its own cache line (dt at 64,
// frame at 72), then columns: Many.P.x at 128 + 1088 = 1216, Player.P.x at
// 5248 + 1088 = 6336.

// A resource read inside a query is loaded once, before the loop; a system
// that writes the resource before the query is seen by it, since the loop
// comes after the write.
// CHECK-LABEL: func.func private @advance(
// CHECK-SAME: %[[W:[^:]*]]: memref<32768xi8>)
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
ecs.system @advance() writes [@P, @Clock] {
  %f = ecs.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %next = arith.addi %f, %one : i64
  ecs.write @Clock "frame", %next : i64
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %dt = ecs.read @Clock "dt" : f32
    %n = arith.addf %x, %dt : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}

ecs.system @drift() reads [@Clock] writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %dt = ecs.read @Clock "dt" : f32
    %n = arith.subf %x, %dt : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}

// A system that writes a resource ends a fused sequence: the two drifts
// are lowered on either side of it, advance stays a call, and dt is loaded
// again after the call rather than reused.
// FUSED-LABEL: func.func @frame(
// FUSED:       memref.load {{.*}} : memref<1xf32>
// FUSED-NEXT:  scf.for
// FUSED:       scf.if
// FUSED:       call @advance(
// FUSED-NOT:   scf.
// FUSED:       memref.load {{.*}} : memref<1xf32>
// FUSED-NEXT:  scf.for
// FUSED:       scf.if
// FUSED:       return
ecs.schedule @frame() {
  ecs.run @drift()
  ecs.run @advance()
  ecs.run @drift()
}
