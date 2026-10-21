// RUN: ent-opt %s --ent-trace | FileCheck %s

// With --trace: a system's (or a schedule's, or a sort's) function is
// timed from its start to each return, a parallel region where it is
// entered and left and each thread's time in it; `main` starts the trace
// and names the sites, the function and the .ent line of the region (sites
// of the same name told apart by their order).

// CHECK-DAG: llvm.mlir.global internal constant @ent_trace_site_0("system s (a.ent:3)\00")
// CHECK-DAG: llvm.mlir.global internal constant @ent_trace_site_1("loop a.ent:4 in s #1\00")
// CHECK-DAG: llvm.mlir.global internal constant @ent_trace_site_2("loop a.ent:4 in s #2\00")

// CHECK-LABEL: func.func private @s(
// CHECK:         %[[S:.*]] = arith.constant 0 : i64
// CHECK:         call @ent_trace_begin(%[[S]])
// CHECK:         %[[L:.*]] = arith.constant 1 : i64
// CHECK:         call @ent_trace_loop_begin(%[[L]])
// CHECK:         omp.parallel {
// CHECK:           %[[T:.*]] = func.call @ent_trace_now()
// CHECK:           func.call @ent_trace_work(%[[L]], %[[T]])
// CHECK-NEXT:      omp.terminator
// CHECK:         call @ent_trace_loop_end(%[[L]])
// CHECK:         call @ent_trace_loop_begin(
// CHECK:         call @ent_trace_loop_end(
// CHECK:         call @ent_trace_end(%[[S]])
// CHECK-NEXT:    return
func.func private @s() attributes {"ent.trace" = "system"} {
  omp.parallel {
    omp.terminator
  } loc("a.ent":4:3)
  omp.parallel {
    omp.terminator
  } loc("a.ent":4:3)
  return
} loc("a.ent":3:1)

// A function that is not timed: only its loops are.
// CHECK-LABEL: func.func private @helper(
// CHECK-NOT:     ent_trace_begin
// CHECK:         return
func.func private @helper() {
  return
}

// CHECK-LABEL: func.func @main(
// CHECK:         call @ent_trace_start()
// CHECK:         llvm.mlir.addressof @ent_trace_site_0
// CHECK:         call @ent_trace_name(
// CHECK:         call @ent_trace_name(
// CHECK:         call @ent_trace_name(
// CHECK-NOT:     call @ent_trace_name(
// CHECK:         return
func.func @main() -> i32 {
  %c0 = arith.constant 0 : i32
  return %c0 : i32
}
