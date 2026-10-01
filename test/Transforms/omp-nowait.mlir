// RUN: ent-opt %s --ent-omp-nowait | FileCheck %s

// The loop is the last thing the region does: the region's closing barrier
// follows, so the loop's own is dropped.
// CHECK-LABEL: func.func @last_loop(
// CHECK:       omp.wsloop nowait {
func.func @last_loop(%n: index, %a: memref<?xf32>, %v: f32) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  omp.parallel {
    omp.wsloop {
      omp.loop_nest (%i) : index = (%c0) to (%n) step (%c1) {
        memref.store %v, %a[%i] : memref<?xf32>
        omp.yield
      }
    }
    omp.terminator
  }
  return
}

// A loop followed by another keeps its barrier: the second loop may read
// what other threads wrote in the first. The last one drops it.
// CHECK-LABEL: func.func @two_loops(
// CHECK:       omp.wsloop {
// CHECK:       omp.wsloop nowait {
func.func @two_loops(%n: index, %a: memref<?xf32>, %v: f32) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  omp.parallel {
    omp.wsloop {
      omp.loop_nest (%i) : index = (%c0) to (%n) step (%c1) {
        memref.store %v, %a[%i] : memref<?xf32>
        omp.yield
      }
    }
    omp.wsloop {
      omp.loop_nest (%i) : index = (%c0) to (%n) step (%c1) {
        %x = memref.load %a[%i] : memref<?xf32>
        omp.yield
      }
    }
    omp.terminator
  }
  return
}

// Sections that end the region drop their barrier the same way.
// CHECK-LABEL: func.func @sections(
// CHECK:       omp.sections nowait {
func.func @sections(%a: memref<?xf32>, %v: f32) {
  %c0 = arith.constant 0 : index
  omp.parallel {
    omp.sections {
      omp.section {
        memref.store %v, %a[%c0] : memref<?xf32>
        omp.terminator
      }
      omp.terminator
    }
    omp.terminator
  }
  return
}
