// RUN: ent-translate --ent-to-c-header %s | FileCheck %s

ent.component @P (x: f32)
ent.resource @Clock (dt: f32, frame: i64)
ent.resource @Wind (strength: f32)
ent.archetype @A (@P) capacity 10

// One count (8 bytes); each resource starts on a cache line with naturally
// aligned fields: Clock.dt at 64, Clock.frame at 72, Wind.strength at 128.
// The entity table's next-slot and free counters follow (136 to 152), and
// a new world zeroes all of that (152 bytes). The first column starts at
// the next 64-byte boundary plus 17 cache lines: 192 + 1088.
// CHECK: memset(arena, 0, 152);
// CHECK: static inline float *ent_A_P_x(ent_world *world) {
// CHECK-NEXT: return (float *)((char *)world + 1280);
// CHECK: // Resource @Clock
// CHECK-NEXT: static inline float *ent_Clock_dt(ent_world *world) {
// CHECK-NEXT: return (float *)((char *)world + 64);
// CHECK: static inline int64_t *ent_Clock_frame(ent_world *world) {
// CHECK-NEXT: return (int64_t *)((char *)world + 72);
// CHECK: // Resource @Wind
// CHECK-NEXT: static inline float *ent_Wind_strength(ent_world *world) {
// CHECK-NEXT: return (float *)((char *)world + 128);
