// Extern fns and procs: C functions that get values, never the world. A
// bool crosses as a byte, a text as the address of a copy on the stack.
// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// A query that calls a proc visits its entities in order, one at a time; a
// fn does not stand in the way of a parallel loop.
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=PARALLEL
// RUN: ent-opt %s --ent-schedule=explain=1 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STAGES
// The whole pipeline accepts the pointer and the calls.
// RUN: ent-opt %s --ent-lower-to-loops --convert-scf-to-cf --convert-to-llvm \
// RUN:     --reconcile-unrealized-casts | mlir-translate --mlir-to-llvmir \
// RUN:   | FileCheck %s --check-prefix=LLVM

ent.component @Name (text: !ent.text<30>, id: i64, odd: i1) capacity 4096
ent.resource @Clock (now: f64)
ent.archetype @Named (@Name) capacity 4096

// CHECK-DAG: func.func private @ent_hash(i64) -> i64
// CHECK-DAG: func.func private @ent_is_odd(i64) -> i8
// CHECK-DAG: func.func private @ent_put(!llvm.ptr, i8)
// CHECK-DAG: func.func private @ent_m_seconds() -> f64
// CHECK-NOT: ent.function
// LLVM-DAG: declare i64 @ent_hash(i64)
// LLVM-DAG: declare i8 @ent_is_odd(i64)
// LLVM-DAG: declare void @ent_put(ptr, i8)
// LLVM-DAG: declare double @ent_m_seconds()
ent.function @hash(i64) -> i64
ent.function @is_odd(i64) -> i1
ent.function proc @put(!ent.text<30>, i1)
ent.function proc @m.seconds() -> f64

// CHECK-LABEL: func.func private @scramble
// CHECK:       scf.for
// CHECK:         %[[H:.*]] = {{.*}}call @ent_hash(%{{.*}}) : (i64) -> i64
// CHECK:         %[[B:.*]] = {{.*}}call @ent_is_odd(%[[H]]) : (i64) -> i8
// CHECK:         arith.trunci %[[B]] : i8 to i1
// PARALLEL-LABEL: func.func private @scramble
// PARALLEL:       scf.parallel
// PARALLEL:         call @ent_hash
ent.system @scramble() {
  ent.query (%n: !ent.ref<@Name, mut>) {
    %id = ent.get %n "id" : <@Name, mut> -> i64
    %h = ent.invoke @hash(%id) : (i64) -> i64
    %odd = ent.invoke @is_odd(%h) : (i64) -> i1
    ent.set %n "id", %h : <@Name, mut>, i64
    ent.set %n "odd", %odd : <@Name, mut>, i1
  }
}

// CHECK-LABEL: func.func private @print
// CHECK:       %[[T:.*]] = call @ent_m_seconds() : () -> f64
// CHECK:       scf.for
// CHECK:         memref.alloca_scope
// CHECK:           %[[SLOT:.*]] = memref.alloca() {alignment = 16 : i64} : memref<i256>
// CHECK:           memref.store %{{.*}}, %[[SLOT]][] : memref<i256>
// CHECK:           %[[P:.*]] = llvm.inttoptr %{{.*}} : i64 to !llvm.ptr
// CHECK:           %[[O:.*]] = arith.extui %{{.*}} : i1 to i8
// CHECK:           {{.*}}call @ent_put(%[[P]], %[[O]]) : (!llvm.ptr, i8) -> ()
// PARALLEL-LABEL: func.func private @print
// PARALLEL-NOT:   scf.parallel
// PARALLEL:       scf.for
// PARALLEL:         call @ent_put
// LLVM-LABEL: define {{.*}}void @print
// LLVM: call ptr @llvm.stacksave
// LLVM: call void @ent_put(ptr %{{.*}}, i8 %{{.*}})
// LLVM: call void @llvm.stackrestore
ent.system @print() {
  %t = ent.invoke proc @m.seconds() : () -> f64
  ent.write @Clock "now", %t : f64
  ent.query (%n: !ent.ref<@Name>) {
    %text = ent.get %n "text" : <@Name> -> !ent.text<30>
    %odd = ent.get %n "odd" : <@Name> -> i1
    ent.invoke proc @put(%text, %odd) : (!ent.text<30>, i1) -> ()
  }
}

ent.system @spawn() {
  %text = ent.read @Clock "now" : f64
}

// A system that calls a proc keeps its place among all others.
// STAGES: @print waits for @scramble: it has effects outside component access ('ent.invoke')
// STAGES: @spawn waits for @print: @print has effects outside component access ('ent.invoke')
ent.schedule @frame() {
  ent.run @scramble()
  ent.run @print()
  ent.run @spawn()
}
