// Functions implemented in C and their calls.
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
