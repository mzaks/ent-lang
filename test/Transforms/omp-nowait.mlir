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

// A loop over 256 pieces whose rows have loops of their own deals them out
// a piece at a time; one over pieces without inner loops, or over a count
// not known (or more than 1024), keeps fixed shares.
// CHECK-LABEL: func.func @schedules(
// CHECK:       omp.wsloop schedule(dynamic) {
// CHECK:       omp.wsloop {
// CHECK:       omp.wsloop {
// CHECK:       omp.wsloop nowait {
func.func @schedules(%n: index, %a: memref<?xf32>, %v: f32) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c256 = arith.constant 256 : index
  omp.parallel {
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%c256) step (%c1) {
        scf.for %r = %c0 to %n step %c1 {
          scf.for %k = %c0 to %r step %c1 {
            memref.store %v, %a[%k] : memref<?xf32>
          }
        }
        omp.yield
      }
    }
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%c256) step (%c1) {
        scf.for %r = %c0 to %n step %c1 {
          memref.store %v, %a[%r] : memref<?xf32>
        }
        omp.yield
      }
    }
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%n) step (%c1) {
        scf.for %r = %c0 to %n step %c1 {
          scf.for %k = %c0 to %r step %c1 {
            memref.store %v, %a[%k] : memref<?xf32>
          }
        }
        omp.yield
      }
    }
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%c256) step (%c1) {
        memref.store %v, %a[%s] : memref<?xf32>
        omp.yield
      }
    }
    omp.terminator
  }
  return
}

// Where a trace chose (`--tune`: the schedule in the loop's location), as
// it chose: dealt out though its rows have no loops of their own, and in
// fixed shares though they have; of a few pieces only.
// CHECK-LABEL: func.func @chosen(
// CHECK:       omp.wsloop schedule(dynamic) {
// CHECK:       omp.wsloop {
// CHECK:       omp.wsloop nowait {
func.func @chosen(%n: index, %a: memref<?xf32>, %v: f32) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c256 = arith.constant 256 : index
  omp.parallel {
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%c256) step (%c1) {
        memref.store %v, %a[%s] : memref<?xf32>
        omp.yield
      }
    } loc(fused<{schedule = "dynamic", site = "a.ent:1:1 f A"}>["a.ent":1:1])
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%c256) step (%c1) {
        scf.for %r = %c0 to %n step %c1 {
          scf.for %k = %c0 to %r step %c1 {
            memref.store %v, %a[%k] : memref<?xf32>
          }
        }
        omp.yield
      }
    } loc(fused<{schedule = "static", site = "a.ent:2:1 f A"}>["a.ent":2:1])
    omp.wsloop {
      omp.loop_nest (%s) : index = (%c0) to (%n) step (%c1) {
        memref.store %v, %a[%s] : memref<?xf32>
        omp.yield
      }
    } loc(fused<{schedule = "dynamic", site = "a.ent:3:1 f A"}>["a.ent":3:1])
    omp.terminator
  }
  return
}
