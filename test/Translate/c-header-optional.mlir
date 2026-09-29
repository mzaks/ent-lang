// RUN: ecs-translate --ecs-to-c-header %s | FileCheck %s
// RUN: mkdir -p %t.dir
// RUN: ecs-translate --ecs-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ecs.component @Q (q: f32)
ecs.component @S (t: f32, u: f32)
ecs.archetype @C (@Q, optional @S) capacity 100

// The optional component's fields are followed by its presence column.
// Growing the count clears the presence of the new rows.
// CHECK:      static inline bool ecs_C_set_count(ecs_world *world, int64_t n) {
// CHECK-NEXT:   if (n < 0 || n > ECS_C_CAPACITY)
// CHECK-NEXT:     return false;
// CHECK-NEXT:   int64_t old = ((int64_t *)world)[0];
// CHECK-NEXT:   if (n > old) {
// CHECK-NEXT:     memset((char *)world + 5760 + old, 0, (size_t)(n - old));
// CHECK-NEXT:   }
// CHECK-NEXT:   ((int64_t *)world)[0] = n;
// CHECK:      static inline float *ecs_C_S_t(ecs_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 2688);
// CHECK:      static inline float *ecs_C_S_u(ecs_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 4224);
// CHECK:      static inline uint8_t *ecs_C_S_present(ecs_world *world) {
// CHECK-NEXT:   return (uint8_t *)((char *)world + 5760);
