// RUN: ent-translate --ent-to-c-header %s --split-input-file | FileCheck %s

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

// -----

// RUN: ent-translate --ent-to-c-header %s --split-input-file \
// RUN:   | FileCheck %s --check-prefix=TYPED
ent.component @N (v: f32) capacity 8
ent.component @Tag ()
ent.archetype @A (@N, optional @Tag) capacity 8
ent.archetype @B (@N) capacity 8
ent.relation @R (w: f32) from @N to @Tag capacity 16

// An end's component may be optional in an archetype: then its presence
// counts.
// TYPED:      static inline bool ent__R_source_has(ent_world *world, ent_entity id) {
// TYPED:        case ENT_ARCHETYPE_A:
// TYPED-NEXT:     return true;
// TYPED-NEXT:   case ENT_ARCHETYPE_B:
// TYPED-NEXT:     return true;
// TYPED:      static inline bool ent__R_target_has(ent_world *world, ent_entity id) {
// TYPED:        case ENT_ARCHETYPE_A:
// TYPED-NEXT:     return ent_A_Tag_present(world)[row] != 0;
// TYPED-NEXT:   default:
// TYPED:      static inline bool ent_R_connect(ent_world *world, ent_entity source,
// TYPED-NEXT:                        ent_entity target, float value_w) {
// TYPED-NEXT:   if (!ent__R_source_has(world, source))
// TYPED-NEXT:     return false;
// TYPED-NEXT:   if (!ent__R_target_has(world, target))
// TYPED-NEXT:     return false;
