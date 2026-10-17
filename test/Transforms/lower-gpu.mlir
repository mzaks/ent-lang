// RUN: ent-opt %s -split-input-file --ent-lower-to-loops="fuse-systems=1 gpu=1" --symbol-dce | FileCheck %s
// RUN: ent-opt %s -split-input-file --ent-lower-to-loops="fuse-systems=1" --symbol-dce | FileCheck %s --check-prefix=CPU

// With `gpu`, the queries of `gpu` systems are loops for the GPU: tiled by
// 256 rows and mapped to blocks and threads, over the GPU's copy of the
// world (ent_gpu_arena). What a run touches of what the GPU keeps is
// where it runs before it (ent_gpu_to_device, ent_gpu_to_host), and what
// it writes is newer there after it (ent_gpu_wrote): here @fall's V.dx,
// which @copy reads on the CPU. Runs of one kind one after another
// (`tick` and @copy) do that once, so that fusion still sees them
// together. The schedule waits for the GPU when it ends.
// Without `gpu` they are loops like any other, and nothing is copied.

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

// CHECK-LABEL: func.func @frame
// CHECK: %[[GPU:.*]] = call @ent_gpu_arena(%arg1)
// CHECK: %[[DX:.*]] = memref.view %[[GPU]][%c6336{{.*}}][]
// CHECK: call @ent_gpu_to_device(%c0_i64, %c6336_i64,
// CHECK: %[[TILE:.*]] = arith.muli %{{.*}}, %c256
// CHECK: scf.parallel (%{{.*}}) = (%{{.*}}) to (%{{.*}}) step (%[[TILE]])
// CHECK: scf.parallel
// CHECK: memref.load %[[DX]]
// CHECK: arith.subf
// CHECK: mapping = [#gpu.loop_dim_map<processor = thread_x
// CHECK: mapping = [#gpu.loop_dim_map<processor = block_x
// CHECK: call @ent_gpu_wrote
// CHECK: call @ent_gpu_to_host(%c0_i64{{.*}}, %c6336_i64
// CHECK-NOT: call @ent_gpu
// CHECK: arith.addi %{{.*}}, %c1_i64
// CHECK: memref.store
// CHECK-NOT: call @ent_gpu
// CHECK: scf.for
// CHECK-NOT: call @ent_gpu_wrote
// CHECK: call @ent_gpu_to_device
// CHECK: scf.parallel
// CHECK: arith.subf
// CHECK: call @ent_gpu_wrote
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK-NEXT: return

// CPU-NOT: ent_gpu_wait
// CPU-NOT: gpu.loop_dim_map
// CPU-LABEL: func.func @frame
// CPU-NOT: ent_gpu_wait
// CPU-NOT: gpu.loop_dim_map

// -----

// A `gpu` system that is not fused: its function has the GPU's copy of
// the world for its loop, and the code around its `for` (uniques) needs
// no copy. An archetype of one entity gets a loop for the GPU too (a
// guard on the CPU), so that its columns stay where the GPU has them.

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
// CHECK: %[[GPU:.*]] = call @ent_gpu_arena(%arg0)
// CHECK: memref.view %[[GPU]]
// CHECK: scf.parallel
// CHECK: scf.parallel
// CHECK: arith.addf
// CHECK: processor = block_x
// CHECK-NOT: call @ent_gpu
// CHECK: memref.store
// CHECK: return
// CHECK-LABEL: func.func @frame
// CHECK: call @ent_gpu_to_device
// CHECK-NEXT: call @move
// CHECK: call @ent_gpu_wrote
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
// CHECK: call @ent_gpu_to_device
// CHECK: scf.parallel
// CHECK: scf.parallel
// CHECK: arith.addf
// CHECK-NEXT: memref.store
// CHECK-NEXT: memref.load
// CHECK-NEXT: arith.mulf
// CHECK: processor = block_x
// CHECK: call @ent_gpu_wrote
// CHECK: call @ent_gpu_to_host
// CHECK: scf.for
// CHECK: arith.subf
// CHECK: call @ent_gpu_wrote
// CHECK-NEXT: call @ent_gpu_wait()
// CHECK-NEXT: return

// One loop for all three on the CPU.
// CPU-LABEL: func.func @frame
// CPU: scf.for
// CPU: arith.addf
// CPU: arith.mulf
// CPU: arith.subf
// CPU-NOT: scf.for
// CPU: return

// -----

// A CPU run that only gives the archetype rows marks only the new rows
// newer on the CPU (the bytes before and after it), and copies nothing
// before it; one under a condition copies only where it holds.

ent.component @P (x: f32)
ent.resource @Show (on: i1)
ent.archetype @A (@P) capacity 1000

ent.system @move() gpu {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %n = arith.addf %x, %x : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}
ent.system @born() {
  %zero = arith.constant 0.0 : f32
  ent.spawn @A(%zero) : f32
}
ent.system @look() {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : !ent.ref<@P> -> f32
  }
}
ent.schedule @frame() {
  ent.run @move()
  ent.run @born()
  ent.run @look() if {
    %c = ent.read @Show "on" : i1
    ent.yield %c : i1
  }
}

// CHECK-LABEL: func.func @frame
// CHECK: scf.parallel
// CHECK: call @ent_gpu_wrote
// CHECK: %[[BEFORE:.*]] = arith.muli
// CHECK-NOT: call @ent_gpu_to_host
// CHECK: call @born
// CHECK: %[[AFTER:.*]] = arith.muli
// CHECK-NEXT: call @ent_gpu_wrote(%c0_i64{{.*}}, %c0_i64{{.*}}, %{{.*}}, %[[BEFORE]], %[[AFTER]])
// CHECK: scf.if
// CHECK-NEXT: arith.constant
// CHECK: func.call @ent_gpu_to_host
// CHECK: call @ent_gpu_wait()
