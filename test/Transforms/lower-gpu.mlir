// RUN: ent-opt %s -split-input-file --ent-lower-to-loops="fuse-systems=1 gpu=1" --symbol-dce | FileCheck %s
// RUN: ent-opt %s -split-input-file --ent-lower-to-loops="fuse-systems=1" --symbol-dce | FileCheck %s --check-prefix=CPU

// With `gpu`, the queries of `gpu` systems are loops for the GPU: tiled by
// 256 rows and mapped to blocks and threads. The CPU waits for them before
// a run that reads what they write, and when the schedule ends; a run
// that touches nothing of theirs (`tick`) does not wait.
// Without `gpu` they are loops like any other, and nothing waits.

ent.component @P (x: f32)
ent.component @V (dx: f32)
ent.resource @Clock (frame: i64)
ent.archetype @A (@P, @V) capacity 1000

ent.system @fall(%dt: f32) gpu {
  ent.query (%v: !ent.ref<@V, mut>) {
    %dx = ent.get %v "dx" : !ent.ref<@V, mut> -> f32
    %n = arith.subf %dx, %dt : f32
    ent.set %v "dx", %n : !ent.ref<@V, mut>, f32
  }
}
ent.system @tick() {
  %f = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %n = arith.addi %f, %one : i64
  ent.write @Clock "frame", %n : i64
}
// On the CPU, and it reads what @fall writes.
ent.system @copy() {
  ent.query (%p: !ent.ref<@P, mut>, %v: !ent.ref<@V>) {
    %dx = ent.get %v "dx" : !ent.ref<@V> -> f32
    ent.set %p "x", %dx : !ent.ref<@P, mut>, f32
  }
}
ent.schedule @frame(%dt: f32) {
  ent.run @fall(%dt) : f32
  ent.run @tick()
  ent.run @copy()
  ent.run @fall(%dt) : f32
}

// CHECK: func.func private @ent_gpu_wait()
// CHECK-LABEL: func.func @frame
// CHECK: %[[TILE:.*]] = arith.muli %{{.*}}, %c256
// CHECK: scf.parallel (%{{.*}}) = (%{{.*}}) to (%{{.*}}) step (%[[TILE]])
// CHECK: scf.parallel
// CHECK: arith.subf
// CHECK: mapping = [#gpu.loop_dim_map<processor = thread_x
// CHECK: mapping = [#gpu.loop_dim_map<processor = block_x
// CHECK-NOT: ent_gpu_wait
// CHECK: arith.addi %{{.*}}, %c1_i64
// CHECK: memref.store
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK: scf.for
// CHECK: scf.parallel
// CHECK: arith.subf
// CHECK: call @ent_gpu_wait()
// CHECK-NEXT: return

// CPU-NOT: ent_gpu_wait
// CPU-NOT: gpu.loop_dim_map
// CPU-LABEL: func.func @frame
// CPU-NOT: ent_gpu_wait
// CPU-NOT: gpu.loop_dim_map

// -----

// A `gpu` system whose code goes on after a query waits for it there; an
// archetype of one entity gets a loop for the GPU too (on the CPU: a
// guard), so that its columns stay where the GPU has them.

ent.component @P (x: f32)
ent.resource @Clock (frame: i64)
ent.archetype @One (@P) capacity 1

ent.system @move() gpu {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %x : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
  %f = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %n = arith.addi %f, %one : i64
  ent.write @Clock "frame", %n : i64
}
ent.schedule @frame() {
  ent.run @move()
}

// CHECK-LABEL: func.func private @move
// CHECK: scf.parallel
// CHECK: scf.parallel
// CHECK: arith.addf
// CHECK: processor = block_x
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK: memref.store
// CHECK-LABEL: func.func @frame
// CHECK-NEXT: call @move
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK-NEXT: return

// CPU-LABEL: func.func private @move
// CPU-NOT: scf.parallel
// CPU: scf.if
// CPU-NOT: ent_gpu_wait

// -----

// Fusion fuses `gpu` systems with each other, not with the CPU's.

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 1000

ent.system @up() gpu {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %x : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.system @down() gpu {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.mulf %x, %x : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.system @here() {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.subf %x, %x : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.schedule @frame() {
  ent.run @up()
  ent.run @down()
  ent.run @here()
}

// CHECK-LABEL: func.func @frame
// CHECK: scf.parallel
// CHECK: scf.parallel
// CHECK: arith.addf
// CHECK-NEXT: memref.store
// CHECK-NEXT: memref.load
// CHECK-NEXT: arith.mulf
// CHECK: processor = block_x
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK: scf.for
// CHECK: arith.subf
// CHECK-NOT: ent_gpu_wait
// CHECK: return

// One loop for all three on the CPU.
// CPU-LABEL: func.func @frame
// CPU: scf.for
// CPU: arith.addf
// CPU: arith.mulf
// CPU: arith.subf
// CPU-NOT: scf.for
// CPU: return
