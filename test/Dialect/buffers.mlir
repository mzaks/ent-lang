// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// CHECK: ent.buffer @Shapes (kind: i32, x: f32, lit: i1) capacity 16
ent.buffer @Shapes (kind: i32, x: f32, lit: i1) capacity 16
ent.component @At (x: f32)
// CHECK: ent.function proc @draw(!ent.buffer<@Shapes>, f32)
ent.function proc @draw(!ent.buffer<@Shapes>, f32)

// A system that says what it reads and writes names a buffer like a
// resource.
// CHECK-LABEL: ent.system @fill(%{{.*}}: f32) reads [@At] writes [@Shapes] {
ent.system @fill(%dx: f32) reads [@At] writes [@Shapes] {
  // CHECK: ent.clear @Shapes
  ent.clear @Shapes
  %k = arith.constant 7 : i32
  %lit = arith.constant true
  // CHECK: ent.append @Shapes(%{{.*}}, %{{.*}}, %{{.*}}) : i32, f32, i1
  ent.append @Shapes (%k, %dx, %lit) : i32, f32, i1
  ent.query (%a: !ent.ref<@At>) {
    %x = ent.get %a "x" : !ent.ref<@At> -> f32
    // CHECK: ent.append @Shapes(%{{.*}}, %{{.*}}, %{{.*}}) : i32, f32, i1
    ent.append @Shapes (%k, %x, %lit) : i32, f32, i1
  }
  // CHECK: %[[N:.*]] = ent.buffer.len @Shapes
  %n = ent.buffer.len @Shapes
  // CHECK: ent.buffer.at @Shapes "x"{{\[}}%[[N]] : i32] : f32
  %v = ent.buffer.at @Shapes "x"[%n : i32] : f32
  // CHECK: %[[S:.*]] = ent.buffer.of @Shapes : <@Shapes>
  %s = ent.buffer.of @Shapes : !ent.buffer<@Shapes>
  // CHECK: ent.invoke proc @draw(%[[S]], %{{.*}}) : (!ent.buffer<@Shapes>, f32) -> ()
  ent.invoke proc @draw(%s, %v) : (!ent.buffer<@Shapes>, f32) -> ()
}
