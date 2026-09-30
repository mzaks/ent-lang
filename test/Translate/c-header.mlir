// RUN: ecs-translate --ecs-to-c-header %s | FileCheck %s
// The header must compile cleanly when a host includes it.
// RUN: mkdir -p %t.dir
// RUN: ecs-translate --ecs-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ecs.component @Kinds (flag: i1, small: i8, count: i32, id: index, mass: f64)
ecs.component @"Odd Name" ("with space": f32)
ecs.archetype @Things (@Kinds, @"Odd Name") capacity 100

// The header holds the count (8 bytes) and the entity table's next-slot
// counter and free-list head (8 each); a new world zeroes those 24 bytes.
// Columns start at the next 64-byte boundary plus 17 cache lines (1088
// bytes) and each is pushed as far past the end of the previous one; the
// archetype's id column follows its component columns, and the entity
// table (a generation and a packed location per slot) comes last.
// CHECK: #define ECS_WORLD_BYTES 16384ull
// CHECK: memset(arena, 0, 24);
// CHECK: typedef uint64_t ecs_entity;
// CHECK: #define ECS_ENTITY_CAPACITY 100
// A location packs the archetype above 7 row bits (capacity 100).
// CHECK: #define ECS__SLOT_BITS 32
// CHECK: #define ECS__ROW_BITS 7
// CHECK: #define ECS__NEXT_SLOT ((int64_t *)((char *)world + 8))
// CHECK: #define ECS__FREE_HEAD ((int64_t *)((char *)world + 16))
// CHECK: #define ECS__GENERATION ((uint32_t *)((char *)world + 12416))
// CHECK: #define ECS__LOCATION ((uint32_t *)((char *)world + 13952))
// Freed slots form a list through their locations.
// CHECK: static inline ecs_entity ecs__allocate(
// CHECK:     *ECS__FREE_HEAD = (int64_t)ECS__LOCATION[slot];
// CHECK: static inline bool ecs_entity_alive(
// CHECK: #define ECS_Things_CAPACITY 100
// CHECK: static inline int64_t ecs_Things_count(const ecs_world *world) {
// CHECK-NEXT: return ((const int64_t *)world)[0];
// CHECK: #define ECS_ARCHETYPE_Things 0
// CHECK: static inline ecs_entity ecs_Things_spawn(ecs_world *world) {
// CHECK-NEXT:   int64_t n = ((int64_t *)world)[0];
// CHECK-NEXT:   if (n >= ECS_Things_CAPACITY)
// CHECK-NEXT:     return ECS_NO_ENTITY;
// CHECK-NEXT:   ecs_entity id = ecs__allocate(world, 0, n);
// CHECK-NEXT:   ((ecs_entity *)((char *)world + 10496))[n] = id;
// CHECK-NEXT:   ((int64_t *)world)[0] = n + 1;
// CHECK: static inline bool ecs_Things_spawn_n(ecs_world *world, int64_t n) {
// CHECK: static inline ecs_entity *ecs_Things_id(ecs_world *world) {
// CHECK-NEXT: return (ecs_entity *)((char *)world + 10496);
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
