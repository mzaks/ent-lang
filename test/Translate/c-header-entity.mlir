// RUN: ecs-translate --ecs-to-c-header %s | FileCheck %s

// Relation fields and entity parameters use the C type of an id.
ecs.component @Link (to: !ecs.entity)
ecs.archetype @Node (@Link) capacity 10

ecs.system @point(%target: !ecs.entity) writes [@Link] {
  ecs.query (%l: !ecs.ref<@Link, mut>) {
    ecs.set %l "to", %target : !ecs.ref<@Link, mut>, !ecs.entity
  }
}

// Nothing is despawned or moved: 32-bit ids.
// CHECK: typedef uint32_t ecs_entity;
// CHECK: static inline ecs_entity *ecs_Node_Link_to(ecs_world *world) {
// CHECK: void _mlir_ciface_frame(ecs_entity arg0, ecs_arena_descriptor *world);
ecs.schedule @frame(%t: !ecs.entity) {
  ecs.run @point(%t) : !ecs.entity
}
