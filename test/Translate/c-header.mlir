// RUN: ent-translate --ent-to-c-header %s | FileCheck %s
// The header must compile cleanly when a host includes it.
// RUN: mkdir -p %t.dir
// RUN: ent-translate --ent-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ent.component @Kinds (flag: i1, small: i8, count: i32, id: index, mass: f64)
ent.component @"Odd Name" ("with space": f32)
ent.archetype @Things (@Kinds, @"Odd Name") capacity 100

// The header holds the count (8 bytes) and two entity counters (8 each); a
// new world zeroes those 24 bytes. Columns start at the next 64-byte
// boundary plus 17 cache lines (1088 bytes) and each is pushed as far past
// the end of the previous one. Nothing is ever despawned or moved, so an id
// is just `archetype << 7 | row` (capacity 100 needs 7 row bits) in 32 bits:
// no id column, no entity table.
// CHECK: #define ENT_WORLD_BYTES 16384ull
// CHECK: memset(arena, 0, 24);
// CHECK: // Entities: nothing is ever despawned or moved
// CHECK: typedef uint32_t ent_entity;
// CHECK: #define ENT_ENTITY_CAPACITY 100
// CHECK: #define ENT__ROW_BITS 7
// CHECK-NOT: ENT__GENERATION
// CHECK-NOT: ENT__LOCATION
// CHECK: static inline ent_entity ent__allocate(
// CHECK:   return ((ent_entity)archetype << ENT__ROW_BITS) | (ent_entity)row;
// CHECK: static inline bool ent_entity_alive(
// CHECK:          (int64_t)row < ent__counts(world)[archetype];
// CHECK: #define ENT_Things_CAPACITY 100
// CHECK: static inline int64_t ent_Things_count(const ent_world *world) {
// CHECK-NEXT: return ((const int64_t *)world)[0];
// CHECK: #define ENT_ARCHETYPE_Things 0
// CHECK: static inline ent_entity ent_Things_spawn(ent_world *world) {
// CHECK-NEXT:   int64_t n = ((int64_t *)world)[0];
// CHECK-NEXT:   if (n >= ENT_Things_CAPACITY)
// CHECK-NEXT:     return ENT_NO_ENTITY;
// CHECK-NEXT:   ent_entity id = ent__allocate(world, 0, n);
// CHECK-NEXT:   ((int64_t *)world)[0] = n + 1;
// CHECK: static inline bool ent_Things_spawn_n(ent_world *world, int64_t n) {
// CHECK-NOT: ent_Things_id
// CHECK: static inline bool *ent_Things_Kinds_flag(ent_world *world) {
// CHECK-NEXT: return (bool *)((char *)world + 1152);
// CHECK: static inline int8_t *ent_Things_Kinds_small(ent_world *world) {
// CHECK-NEXT: return (int8_t *)((char *)world + 2368);
// CHECK: static inline int32_t *ent_Things_Kinds_count(ent_world *world) {
// CHECK-NEXT: return (int32_t *)((char *)world + 3584);
// CHECK: static inline int64_t *ent_Things_Kinds_id(ent_world *world) {
// CHECK-NEXT: return (int64_t *)((char *)world + 5120);
// CHECK: static inline double *ent_Things_Kinds_mass(ent_world *world) {
// CHECK-NEXT: return (double *)((char *)world + 7040);
// CHECK: static inline float *ent_Things_Odd_Name_with_space(ent_world *world) {
// CHECK-NEXT: return (float *)((char *)world + 8960);

ent.system @touch(%m: f64, %n: i32) writes [@Kinds] {
  ent.query (%k: !ent.ref<@Kinds, mut>) {
    ent.set %k "mass", %m : !ent.ref<@Kinds, mut>, f64
    ent.set %k "count", %n : !ent.ref<@Kinds, mut>, i32
  }
}

// CHECK: void _mlir_ciface_step(double arg0, int32_t arg1, ent_arena_descriptor *world);
// CHECK: static inline void ent_step(ent_world *world, double arg0, int32_t arg1) {
// CHECK: _mlir_ciface_step(arg0, arg1, &arena);
ent.schedule @step(%m: f64, %n: i32) {
  ent.run @touch(%m, %n) : f64, i32
}
