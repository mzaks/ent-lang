// Functions, the program's own and those implemented in C, and their calls.
// RUN: ent-opt %s | ent-opt | FileCheck %s
// RUN: ent-opt %s --ent-print-access -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ACCESS

ent.component @Name (text: !ent.text<30>, id: i64)
ent.archetype @Named (@Name) capacity 8

// CHECK: ent.function @hash(i64) -> i64
ent.function @hash(i64) -> i64
// CHECK: ent.function proc @put(!ent.text<30>, i1)
ent.function proc @put(!ent.text<30>, i1)
// CHECK: ent.function proc @seconds() -> f64
ent.function proc @seconds() -> f64

// With a body: the program's own. It may call other fns, and itself.
// CHECK:      ent.function @next(%[[ID:.*]]: i64) -> i64 {
// CHECK-NEXT:   %[[H:.*]] = ent.invoke @hash(%[[ID]]) : (i64) -> i64
// CHECK-NEXT:   ent.yield %[[H]] : i64
// CHECK-NEXT: }
ent.function @next(%id: i64) -> i64 {
  %h = ent.invoke @hash(%id) : (i64) -> i64
  ent.yield %h : i64
}
// CHECK:      ent.function @one() -> i32 {
// CHECK-NEXT:   %[[ONE:.*]] = arith.constant 1 : i32
// CHECK-NEXT:   ent.yield %[[ONE]] : i32
// CHECK-NEXT: }
ent.function @one() -> i32 {
  %one = arith.constant 1 : i32
  ent.yield %one : i32
}
// Any value goes in and comes out, a text too.
// CHECK: ent.function @same(%{{.*}}: !ent.text<30>, %{{.*}}: !ent.entity) -> !ent.text<30> {
ent.function @same(%text: !ent.text<30>, %e: !ent.entity) -> !ent.text<30> {
  ent.yield %text : !ent.text<30>
}
// Several values: the function's results, and those of its calls.
// CHECK:      ent.function @swap(%[[A:.*]]: i32, %[[B:.*]]: f32) -> (f32, i32) {
// CHECK-NEXT:   ent.yield %[[B]], %[[A]] : f32, i32
// CHECK-NEXT: }
ent.function @swap(%a: i32, %b: f32) -> (f32, i32) {
  ent.yield %b, %a : f32, i32
}
// CHECK:      ent.function @back(%[[A:.*]]: i32, %[[B:.*]]: f32) -> (i32, f32) {
// CHECK-NEXT:   %[[X:.*]]:2 = ent.invoke @swap(%[[A]], %[[B]]) : (i32, f32) -> (f32, i32)
// CHECK-NEXT:   ent.yield %[[X]]#1, %[[X]]#0 : i32, f32
ent.function @back(%a: i32, %b: f32) -> (i32, f32) {
  %x, %y = ent.invoke @swap(%a, %b) : (i32, f32) -> (f32, i32)
  ent.yield %y, %x : i32, f32
}
// An enum: a value is one of its cases. It goes to C and comes back.
// CHECK: ent.enum @Way ["Left", "Right"]
ent.enum @Way ["Left", "Right"]
// CHECK: ent.function @turn(!ent.enum<@Way>) -> !ent.enum<@Way>
ent.function @turn(!ent.enum<@Way>) -> !ent.enum<@Way>
// CHECK:      ent.function @gcd(%[[A:.*]]: i64, %[[B:.*]]: i64) -> i64 {
// CHECK:          %{{.*}} = ent.invoke @gcd(%[[B]], %{{.*}}) : (i64, i64) -> i64
ent.function @gcd(%a: i64, %b: i64) -> i64 {
  %zero = arith.constant 0 : i64
  %done = arith.cmpi eq, %b, %zero : i64
  %r = scf.if %done -> (i64) {
    scf.yield %a : i64
  } else {
    %m = arith.remsi %a, %b : i64
    %g = ent.invoke @gcd(%b, %m) : (i64, i64) -> i64
    scf.yield %g : i64
  }
  ent.yield %r : i64
}

// A fn is computing: the system's access is what its body does.
// ACCESS: remark: reads Named.Name.id, Named.count; writes Named.Name.id
ent.system @scramble() {
  ent.query (%n: !ent.ref<@Name, mut>) {
    %id = ent.get %n "id" : <@Name, mut> -> i64
    // CHECK: %{{.*}} = ent.invoke @hash(%{{.*}}) : (i64) -> i64
    %h = ent.invoke @hash(%id) : (i64) -> i64
    ent.set %n "id", %h : <@Name, mut>, i64
  }
}

// A proc acts on the outside: its system keeps its place among all others.
// ACCESS: remark: reads Named.Name.text, Named.count; writes nothing
// ACCESS: [[@LINE+3]]:8: note: has effects outside component access, so the system conflicts with every other system
ent.system @print() {
  // CHECK: %{{.*}} = ent.invoke proc @seconds() : () -> f64
  %t = ent.invoke proc @seconds() : () -> f64
  ent.query (%n: !ent.ref<@Name>) {
    %text = ent.get %n "text" : <@Name> -> !ent.text<30>
    %yes = arith.constant true
    // CHECK: ent.invoke proc @put(%{{.*}}, %{{.*}}) : (!ent.text<30>, i1) -> ()
    ent.invoke proc @put(%text, %yes) : (!ent.text<30>, i1) -> ()
  }
}
