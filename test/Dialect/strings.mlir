// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// A text of any length: a field's type, and the ops that make, look into
// and keep one.
// CHECK: ent.component @Label (name: !ent.string, short: !ent.text<14>)
ent.component @Label (name: !ent.string, short: !ent.text<14>)
ent.archetype @Labelled (@Label) capacity 8

// CHECK-LABEL: ent.function @longest(%{{.*}}: !ent.string, %{{.*}}: !ent.text<14>) -> i32 {
// CHECK:   %[[N:.*]] = ent.text.length %[[A:.*]]
// CHECK:   %[[B:.*]] = ent.text.of %{{.*}} : <14>
// CHECK:   %{{.*}} = ent.text.equal %[[A]], %[[B]]
// CHECK:   %{{.*}} = ent.text.at %[[A]], %[[N]] : i32
// CHECK:   %{{.*}} = ent.text.cut %[[A]] : <14>
ent.function @longest(%a: !ent.string, %b: !ent.text<14>) -> i32 {
  %n = ent.text.length %a
  %view = ent.text.of %b : !ent.text<14>
  %same = ent.text.equal %a, %view
  %byte = ent.text.at %a, %n : i32
  %cut = ent.text.cut %a : !ent.text<14>
  ent.yield %n : i32
}

// CHECK-LABEL: ent.system @rename() {
// CHECK:     %[[NEW:.*]] = ent.text.constant "another name"
// CHECK:     %[[OWN:.*]] = ent.text.own %[[NEW]]
// CHECK:     ent.text.drop %{{.*}}
ent.system @rename() {
  ent.query (%l: !ent.ref<@Label, mut>) {
    %new = ent.text.constant "another name"
    %old = ent.get %l "name" : !ent.ref<@Label, mut> -> !ent.string
    %own = ent.text.own %new
    ent.set %l "name", %own : !ent.ref<@Label, mut>, !ent.string
    ent.text.drop %old
  }
}
