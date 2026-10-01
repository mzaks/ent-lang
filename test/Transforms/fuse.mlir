// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce | FileCheck %s

ent.component @P (x: f32)
ent.component @V (dx: f32)
ent.archetype @A (@P, @V) capacity 1000
ent.archetype @B (@P) capacity 1000

func.func private @log()

// Fused systems are no longer called, so symbol-dce removes them; the
// opaque one is still called and stays. System functions are emitted where
// the systems were declared, ahead of the schedules.
// CHECK-NOT: func.func private @accelerate
// CHECK-NOT: func.func private @move
// CHECK-NOT: func.func private @shift
// CHECK: func.func private @logs

ent.system @accelerate(%k: f32) writes [@V] {
  // Ops outside queries are cloned once per run, with parameters mapped to
  // the run's arguments.
  %twice = arith.addf %k, %k : f32
  ent.query (%v: !ent.ref<@V, mut>) {
    %dx = ent.get %v "dx" : !ent.ref<@V, mut> -> f32
    %n = arith.addf %dx, %twice : f32
    ent.set %v "dx", %n : !ent.ref<@V, mut>, f32
  }
}
// Reads what accelerate writes: they conflict, and still fuse.
ent.system @move() reads [@V] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>, %v: !ent.ref<@V>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %dx = ent.get %v "dx" : !ent.ref<@V> -> f32
    %n = arith.addf %x, %dx : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.system @shift(%d: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %d : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.system @logs() {
  func.call @log() : () -> ()
}

// One loop per archetype; each holds, in program order, the bodies of the
// queries that match it. Stages are dissolved first. Columns: A.P.x at
// 1152, A.V.dx at 6272, B.P.x at 11392 (no id columns: nothing is
// despawned or moved).
// CHECK-LABEL: func.func @frame(
// CHECK-SAME: %[[K:[^:]*]]: f32, %[[D:[^:]*]]: f32, %[[W:[^:]*]]: memref<16384xi8>)
// CHECK:      arith.constant 6272
// CHECK-NEXT: %[[ADX:.*]] = memref.view %[[W]]
// CHECK:      arith.constant 1152
// CHECK-NEXT: %[[AX:.*]] = memref.view %[[W]]
// CHECK:      arith.constant 11392
// CHECK-NEXT: %[[BX:.*]] = memref.view %[[W]]
// CHECK:      %[[TWICE:.*]] = arith.addf %[[K]], %[[K]]
// CHECK:      %[[NA:.*]] = arith.index_cast
// CHECK:      scf.for %[[I:.*]] = %{{.*}} to %[[NA]]
// CHECK-NEXT:   %[[DX:.*]] = memref.load %[[ADX]][%[[I]]]
// CHECK-NEXT:   %[[DX2:.*]] = arith.addf %[[DX]], %[[TWICE]]
// CHECK-NEXT:   memref.store %[[DX2]], %[[ADX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[AX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[ADX]][%[[I]]]
// CHECK-NEXT:   arith.addf
// CHECK-NEXT:   memref.store %{{.*}}, %[[AX]][%[[I]]]
// CHECK-NEXT:   memref.load %[[AX]][%[[I]]]
// CHECK-NEXT:   arith.addf %{{.*}}, %[[D]]
// CHECK-NEXT:   memref.store %{{.*}}, %[[AX]][%[[I]]]
// CHECK-NEXT: }
// CHECK:      %[[NB:.*]] = arith.index_cast
// CHECK:      scf.for %[[J:.*]] = %{{.*}} to %[[NB]]
// CHECK-NEXT:   memref.load %[[BX]][%[[J]]]
// CHECK-NEXT:   arith.addf %{{.*}}, %[[D]]
// CHECK-NEXT:   memref.store %{{.*}}, %[[BX]][%[[J]]]
// CHECK-NEXT: }
// CHECK-NEXT: return
ent.schedule @frame(%k: f32, %d: f32) {
  ent.stage {
    ent.run @accelerate(%k) : f32
  }
  ent.stage {
    ent.run @move()
    ent.run @shift(%d) : f32
  }
}

// An opaque system and an op with effects both end a fused sequence. @shift
// matches both archetypes, so each sequence is two loops (A, then B).
// CHECK-LABEL: func.func @barriers(
// CHECK-SAME: %{{[^:]*}}: f32, %[[W:[^:]*]]: memref<16384xi8>)
// CHECK:      scf.for
// CHECK:      scf.for
// CHECK:      }
// CHECK-NEXT: call @logs(%[[W]])
// CHECK:      scf.for
// CHECK:      scf.for
// CHECK:      }
// CHECK-NEXT: call @log()
// CHECK:      scf.for
// CHECK:      scf.for
// CHECK:      }
// CHECK-NEXT: return
ent.schedule @barriers(%d: f32) {
  ent.run @shift(%d) : f32
  ent.run @logs()
  ent.run @shift(%d) : f32
  func.call @log() : () -> ()
  ent.run @shift(%d) : f32
}
