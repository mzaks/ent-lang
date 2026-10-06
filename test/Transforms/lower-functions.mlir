// Extern fns and procs: C functions that get values, never the world. A
// bool crosses as a byte, a text as the address of a copy on the stack.
// A fn with a body becomes a function of the program's, which gets its
// values as they are.
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

// CHECK-LABEL: func.func private @ent_twice(
// CHECK-SAME:    %[[X:.*]]: i64, %[[UP:.*]]: i1) -> i64
// CHECK:         %[[H:.*]] = call @ent_hash(%[[X]]) : (i64) -> i64
// CHECK:         %[[R:.*]] = scf.if %[[UP]] -> (i64)
// CHECK:           call @ent_twice(%[[H]], %{{.*}}) : (i64, i1) -> i64
// CHECK:         return %[[R]] : i64
// LLVM-LABEL: define {{.*}}i64 @ent_twice(i64 %{{.*}}, i1 %{{.*}})
ent.function @twice(%x: i64, %up: i1) -> i64 {
  %h = ent.invoke @hash(%x) : (i64) -> i64
  %r = scf.if %up -> (i64) {
    %no = arith.constant false
    %again = ent.invoke @twice(%h, %no) : (i64, i1) -> i64
    scf.yield %again : i64
  } else {
    scf.yield %h : i64
  }
  ent.yield %r : i64
}
// A text is the integer that holds it, in and out.
// CHECK-LABEL: func.func private @ent_same(
// CHECK-SAME:    %[[T:.*]]: i256) -> i256
// CHECK:         return %[[T]] : i256
ent.function @same(%text: !ent.text<30>) -> !ent.text<30> {
  ent.yield %text : !ent.text<30>
}

// Several values are several results.
// CHECK-LABEL: func.func private @ent_both(
// CHECK-SAME:    %[[X:.*]]: i64) -> (i64, i1)
// CHECK:         return %[[X]], %{{.*}} : i64, i1
// LLVM-LABEL: define {{.*}}{ i64, i1 } @ent_both(i64 %{{.*}})
ent.function @both(%x: i64) -> (i64, i1) {
  %yes = arith.constant true
  ent.yield %x, %yes : i64, i1
}
// An enum is its byte, in the program's own functions and in C's.
ent.enum @Way ["Left", "Right"]
// CHECK-DAG: func.func private @ent_turn(i8) -> i8
// LLVM-DAG: declare i8 @ent_turn(i8)
ent.function @turn(!ent.enum<@Way>) -> !ent.enum<@Way>
// CHECK-LABEL: func.func private @ent_twice_turned(
// CHECK-SAME:    %[[W:.*]]: i8) -> i8
// CHECK:         %[[A:.*]] = call @ent_turn(%[[W]]) : (i8) -> i8
// CHECK:         %[[B:.*]] = call @ent_turn(%[[A]]) : (i8) -> i8
// CHECK:         return %[[B]] : i8
ent.function @twice_turned(%w: !ent.enum<@Way>) -> !ent.enum<@Way> {
  %a = ent.invoke @turn(%w) : (!ent.enum<@Way>) -> !ent.enum<@Way>
  %b = ent.invoke @turn(%a) : (!ent.enum<@Way>) -> !ent.enum<@Way>
  ent.yield %b : !ent.enum<@Way>
}

// CHECK-LABEL: func.func private @scramble
// CHECK:       scf.for
// CHECK:         %[[H0:.*]] = {{.*}}call @ent_hash(%{{.*}}) : (i64) -> i64
// CHECK:         %[[H1:.*]]:2 = {{.*}}call @ent_both(%[[H0]]) : (i64) -> (i64, i1)
// CHECK:         %[[H:.*]] = {{.*}}call @ent_twice(%[[H1]]#0, %[[H1]]#1) : (i64, i1) -> i64
// CHECK:         %[[B:.*]] = {{.*}}call @ent_is_odd(%[[H]]) : (i64) -> i8
// CHECK:         arith.trunci %[[B]] : i8 to i1
// PARALLEL-LABEL: func.func private @scramble
// PARALLEL:       scf.parallel
// PARALLEL:         call @ent_hash
// PARALLEL:         call @ent_twice
ent.system @scramble() {
  ent.query (%n: !ent.ref<@Name, mut>) {
    %id = ent.get %n "id" : <@Name, mut> -> i64
    %h0 = ent.invoke @hash(%id) : (i64) -> i64
    %yes = arith.constant true
    %h1, %flag = ent.invoke @both(%h0) : (i64) -> (i64, i1)
    %h = ent.invoke @twice(%h1, %flag) : (i64, i1) -> i64
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
    %text0 = ent.get %n "text" : <@Name> -> !ent.text<30>
    %text = ent.invoke @same(%text0) : (!ent.text<30>) -> !ent.text<30>
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
