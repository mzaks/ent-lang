// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// CHECK: ent.component @N (v: f32, input: f32) capacity 8
// CHECK: ent.relation @Syn (w: f32) capacity 64
// CHECK: ent.relation @Link () capacity 16
ent.component @N (v: f32, input: f32) capacity 8
ent.relation @Syn (w: f32) capacity 64
ent.relation @Link () capacity 16

// CHECK-LABEL: ent.system @push() {
// CHECK:   ent.edges @Syn out (%[[S:.*]]: !ent.ref<@Syn>, %[[T:.*]]: !ent.entity) {
// CHECK:     %[[W:.*]] = ent.get %[[S]] "w" : <@Syn> -> f32
// CHECK:     ent.apply %[[T]] @N "input" add %[[W]] : f32
ent.system @push() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn out (%s: !ent.ref<@Syn>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      ent.apply %t @N "input" add %w : f32
    }
  }
}

// CHECK-LABEL: ent.system @learn() reads [@N] writes [@Syn] {
// CHECK:   ent.edges @Syn in (%[[S:.*]]: !ent.ref<@Syn, mut>, %{{.*}}: !ent.entity) {
// CHECK:     ent.set %[[S]] "w", %{{.*}} : <@Syn, mut>, f32
// CHECK:     ent.disconnect
ent.system @learn() reads [@N] writes [@Syn] {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn, mut>, %p: !ent.entity) {
      %zero = arith.constant 0.0 : f32
      ent.set %s "w", %zero : !ent.ref<@Syn, mut>, f32
      ent.disconnect
    }
  }
}

// CHECK-LABEL: ent.system @wire(
// CHECK:   ent.connect @Syn %{{.*}}, %{{.*}} (%{{.*}}) : f32
// CHECK:   ent.connect @Link %{{.*}}, %{{.*}} ()
// CHECK:   ent.query
// CHECK:     ent.connect @Link %{{.*}}, %{{.*}} ()
ent.system @wire(%a: !ent.entity, %b: !ent.entity) {
  %w = arith.constant 1.0 : f32
  ent.connect @Syn %a, %b (%w) : f32
  ent.connect @Link %b, %a ()
  ent.query (%n: !ent.ref<@N>) {
    %self = ent.entity
    ent.connect @Link %self, %a ()
  }
}
