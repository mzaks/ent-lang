// RUN: ent-translate --ent-to-c-header %s | FileCheck %s

ent.component @P (x: f32)
ent.resource @Pick (count: i32, target: !ent.entity, other: !ent.entity)
ent.archetype @A (@P) capacity 10

// A unique's entity fields start as no entity (all ones), not as the
// first entity, as its numbers start as 0: set after the header is zeroed.
// CHECK: memset(arena, 0,
// CHECK-NEXT: memset((char *)arena + [[T:[0-9]+]], 0xff, {{4|8}});
// CHECK-NEXT: memset((char *)arena + {{[0-9]+}}, 0xff, {{4|8}});
// CHECK: static inline {{.*}}ent_Pick_target(ent_world *world) {
// CHECK-NEXT: return {{.*}}((char *)world + [[T]]);
