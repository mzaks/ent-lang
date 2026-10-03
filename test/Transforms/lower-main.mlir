// The entry point becomes a C `main` that creates the world like the
// generated header does and runs the body: schedule calls as calls of the
// lowered schedules, loops as `scf.while` that test after each run.
// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops --convert-scf-to-cf --convert-to-llvm \
// RUN:     --reconcile-unrealized-casts | mlir-translate --mlir-to-llvmir \
// RUN:   | FileCheck %s --check-prefix=LLVM

ent.component @Position (x: f32)
ent.resource @Clock (frame: i32, dt: f32)
ent.resource @Exit (value: i1)
ent.archetype @Rock (@Position) capacity 8

ent.system @tick() {
  %f = ent.read @Clock "frame" : i32
  ent.write @Clock "frame", %f : i32
}
ent.schedule @setup() {
  ent.run @tick()
}
ent.schedule @frame(%dt: f32) {
  ent.run @tick()
}

// The arena, aligned as the header aligns it, with its header (counts,
// resources, entity counters) zeroed; columns stay untouched.
// CHECK-LABEL: func.func @main() -> i32 {
// CHECK-NEXT:   %[[WORLD:.*]] = memref.alloc() {alignment = 16384 : i64} : memref<[[BYTES:.*]]xi8>
// CHECK:        scf.for %[[I:.*]] = %{{.*}} to %{{.*}} step
// CHECK-NEXT:     memref.store %{{.*}}, %[[WORLD]][%[[I]]]
// CHECK:        call @ent.main(%[[WORLD]])
// CHECK-NEXT:   memref.dealloc %[[WORLD]]
// CHECK-NEXT:   %[[STATUS:.*]] = arith.constant 0 : i32
// CHECK-NEXT:   return %[[STATUS]] : i32

// CHECK: func.func private @ent.main(%[[ARENA:.*]]: memref<[[BYTES]]xi8>) {
// CHECK:        call @setup(%[[ARENA]])
// CHECK-NEXT:   scf.while : () -> () {
// CHECK:          %[[DT:.*]] = memref.load
// CHECK-NEXT:     call @frame(%[[DT]], %[[ARENA]])
// A loop without a condition repeats forever.
// CHECK-NEXT:     scf.while : () -> () {
// CHECK-NEXT:       call @setup(%[[ARENA]])
// CHECK-NEXT:       %[[FOREVER:.*]] = arith.constant true
// CHECK-NEXT:       scf.condition(%[[FOREVER]])
// CHECK-NEXT:     } do {
// CHECK-NEXT:       scf.yield
// CHECK-NEXT:     }
// It goes on while the body did not yield true.
// CHECK:          %[[DONE:.*]] = memref.load
// CHECK-NEXT:     %[[TRUE:.*]] = arith.constant true
// CHECK-NEXT:     %[[PROCEED:.*]] = arith.xori %[[DONE]], %[[TRUE]] : i1
// CHECK-NEXT:     scf.condition(%[[PROCEED]])
// CHECK-NEXT:   } do {
// CHECK-NEXT:     scf.yield
// CHECK-NEXT:   }
// CHECK-NEXT:   return
// CHECK-NOT: ent.

// LLVM: define i32 @main()
ent.main {
  ent.call @setup()
  ent.loop {
    %dt = ent.read @Clock "dt" : f32
    ent.call @frame(%dt) : f32
    ent.loop {
      ent.call @setup()
    }
    %done = ent.read @Exit "value" : i1
    ent.yield %done : i1
  }
}
