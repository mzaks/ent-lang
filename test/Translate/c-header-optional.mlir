// RUN: ecs-translate --ecs-to-c-header %s | FileCheck %s
// RUN: mkdir -p %t.dir
// RUN: ecs-translate --ecs-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ecs.component @Q (q: f32)
ecs.component @S (t: f32, u: f32)
ecs.archetype @C (@Q, optional @S) capacity 100

// The optional component's fields are followed by its presence column.
// A spawned entity starts without the optional component.
// CHECK:      static inline ecs_entity ecs_C_spawn(ecs_world *world) {
// CHECK-NEXT:   int64_t n = ((int64_t *)world)[0];
// CHECK-NEXT:   if (n >= ECS_C_CAPACITY)
// CHECK-NEXT:     return ECS_NO_ENTITY;
// CHECK-NEXT:   ((uint8_t *)((char *)world + 5760))[n] = 0;
// CHECK-NEXT:   ecs_entity id = ecs__allocate(world, 0, n);
// CHECK:      static inline float *ecs_C_S_t(ecs_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 2688);
// CHECK:      static inline float *ecs_C_S_u(ecs_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 4224);
// CHECK:      static inline uint8_t *ecs_C_S_present(ecs_world *world) {
// CHECK-NEXT:   return (uint8_t *)((char *)world + 5760);
