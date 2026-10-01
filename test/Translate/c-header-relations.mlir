// RUN: ent-translate --ent-to-c-header %s | FileCheck %s

ent.component @N (v: f32) capacity 8
ent.archetype @A (@N) capacity 8
ent.relation @Syn (w: f32, delay: i32) capacity 64

// The edge count and clean flag are in the header, which a new world
// zeroes: no edges, and unclean, so the first frame sorts.
// CHECK: // Relation @Syn: edges from a source to a target entity.
// CHECK: #define ENT_Syn_CAPACITY 64
// CHECK: static inline int64_t ent_Syn_count(ent_world *world) {
// CHECK: static inline ent_entity *ent_Syn_source(ent_world *world) {
// CHECK: static inline ent_entity *ent_Syn_target(ent_world *world) {
// CHECK: static inline float *ent_Syn_w(ent_world *world) {
// CHECK: static inline int32_t *ent_Syn_delay(ent_world *world) {
// CHECK:      static inline bool ent_Syn_connect(ent_world *world, ent_entity source,
// CHECK-NEXT:                        ent_entity target, float value_w, int32_t value_delay) {
// CHECK-NEXT:   int64_t *count = ent__Syn_count(world);
// CHECK-NEXT:   if (*count >= ENT_Syn_CAPACITY)
// CHECK-NEXT:     return false;
// CHECK-NEXT:   ent_Syn_source(world)[*count] = source;
// CHECK-NEXT:   ent_Syn_target(world)[*count] = target;
// CHECK-NEXT:   ent_Syn_w(world)[*count] = value_w;
// CHECK-NEXT:   ent_Syn_delay(world)[*count] = value_delay;
// CHECK-NEXT:   ++*count;
// CHECK-NEXT:   = 0; // unclean
