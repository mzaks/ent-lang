// RUN: ent-translate --ent-to-c-header %s | FileCheck %s

// Relation fields and entity parameters use the C type of an id.
ent.component @Link (to: !ent.entity)
ent.archetype @Node (@Link) capacity 10

ent.system @point(%target: !ent.entity) writes [@Link] {
  ent.query (%l: !ent.ref<@Link, mut>) {
    ent.set %l "to", %target : !ent.ref<@Link, mut>, !ent.entity
  }
}

// Nothing is despawned or moved: 32-bit ids.
// CHECK: typedef uint32_t ent_entity;
// CHECK: static inline ent_entity *ent_Node_Link_to(ent_world *world) {
// CHECK: void _mlir_ciface_frame(ent_entity arg0, ent_arena_descriptor *world);
ent.schedule @frame(%t: !ent.entity) {
  ent.run @point(%t) : !ent.entity
}
