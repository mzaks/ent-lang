// RUN: ecs-translate --ecs-to-c-header %s | FileCheck %s
// The header must compile cleanly when a host includes it.
// RUN: mkdir -p %t.dir
// RUN: ecs-translate --ecs-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ecs.component @Kinds (flag: i1, small: i8, count: i32, id: index, mass: f64)
ecs.component @"Odd Name" ("with space": f32)
ecs.archetype @Things (@Kinds, @"Odd Name") capacity 100

// Counts (8 bytes) round up to 64, then each column is pushed 17 cache
// lines (1088 bytes) past the end of the previous one.
// CHECK: #define ECS_WORLD_BYTES 16384ull
// CHECK: memset(arena, 0, 8);
// CHECK: #define ECS_Things_CAPACITY 100
// CHECK: static inline int64_t ecs_Things_count(const ecs_world *world) {
// CHECK-NEXT: return ((const int64_t *)world)[0];
// CHECK: static inline bool ecs_Things_set_count(ecs_world *world, int64_t n) {
// CHECK-NEXT: if (n < 0 || n > ECS_Things_CAPACITY)
// CHECK: static inline bool *ecs_Things_Kinds_flag(ecs_world *world) {
// CHECK-NEXT: return (bool *)((char *)world + 1152);
// CHECK: static inline int8_t *ecs_Things_Kinds_small(ecs_world *world) {
// CHECK-NEXT: return (int8_t *)((char *)world + 2368);
// CHECK: static inline int32_t *ecs_Things_Kinds_count(ecs_world *world) {
// CHECK-NEXT: return (int32_t *)((char *)world + 3584);
// CHECK: static inline int64_t *ecs_Things_Kinds_id(ecs_world *world) {
// CHECK-NEXT: return (int64_t *)((char *)world + 5120);
// CHECK: static inline double *ecs_Things_Kinds_mass(ecs_world *world) {
// CHECK-NEXT: return (double *)((char *)world + 7040);
// CHECK: static inline float *ecs_Things_Odd_Name_with_space(ecs_world *world) {
// CHECK-NEXT: return (float *)((char *)world + 8960);

ecs.system @touch(%m: f64, %n: i32) writes [@Kinds] {
  ecs.query (%k: !ecs.ref<@Kinds, mut>) {
    ecs.set %k "mass", %m : !ecs.ref<@Kinds, mut>, f64
    ecs.set %k "count", %n : !ecs.ref<@Kinds, mut>, i32
  }
}

// CHECK: void _mlir_ciface_step(double arg0, int32_t arg1, ecs_arena_descriptor *world);
// CHECK: static inline void ecs_step(ecs_world *world, double arg0, int32_t arg1) {
// CHECK: _mlir_ciface_step(arg0, arg1, &arena);
ecs.schedule @step(%m: f64, %n: i32) {
  ecs.run @touch(%m, %n) : f64, i32
}
