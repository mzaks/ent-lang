// RUN: ent-translate --ent-to-c-header %s | FileCheck %s
// RUN: mkdir -p %t.dir
// RUN: ent-translate --ent-to-c-header %s -o %t.dir/world.h
// RUN: echo '#include "world.h"' > %t.dir/use.c
// RUN: clang -fsyntax-only -Wall -Wextra -Werror -I%t.dir %t.dir/use.c

ent.component @Q (q: f32)
ent.component @S (t: f32, u: f32)
ent.archetype @C (@Q, optional @S) capacity 100

// The optional component's fields are followed by its presence column.
// A spawned entity starts without the optional component.
// CHECK:      static inline ent_entity ent_C_spawn(ent_world *world) {
// CHECK-NEXT:   int64_t n = ((int64_t *)world)[0];
// CHECK-NEXT:   if (n >= ENT_C_CAPACITY)
// CHECK-NEXT:     return ENT_NO_ENTITY;
// CHECK-NEXT:   ((uint8_t *)((char *)world + 5760))[n] = 0;
// CHECK-NEXT:   ent_entity id = ent__allocate(world, 0, n);
// CHECK:      static inline float *ent_C_S_t(ent_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 2688);
// CHECK:      static inline float *ent_C_S_u(ent_world *world) {
// CHECK-NEXT:   return (float *)((char *)world + 4224);
// CHECK:      static inline uint8_t *ent_C_S_present(ent_world *world) {
// CHECK-NEXT:   return (uint8_t *)((char *)world + 5760);
