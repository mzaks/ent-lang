// RUN: ecs-translate --ecs-to-c-header %s -split-input-file | FileCheck %s

// The entity scheme follows from the structural changes and capacities.

// Moves but no despawns: an id is a slot (2000 slots need 11 bits), the
// table holds only locations, and slots are never reused.
// CHECK: // Entities: entities move but are never despawned
// CHECK: typedef uint32_t ecs_entity;
// CHECK: #define ECS__SLOT_BITS 11
// CHECK-NOT: ECS__GENERATION
// CHECK: #define ECS__LOCATION ((uint32_t *)
// CHECK: static inline ecs_entity ecs__allocate(
// CHECK:        int64_t slot = (*ECS__NEXT_SLOT)++;
// CHECK: return id != ECS_NO_ENTITY && (int64_t)ecs__slot(id) < *ECS__NEXT_SLOT;
// CHECK: static inline ecs_entity *ecs_Calm_id(
ecs.component @P (x: f32)
ecs.component @S (s: f32)
ecs.archetype @Calm (@P) capacity 1000
ecs.archetype @Stunned (@P, @S) capacity 1000
ecs.system @stun(%s: f32) reads [@P] writes [@S] {
  ecs.query (%p: !ecs.ref<@P>) {
    ecs.add @S(%s) : f32
  }
}

// -----

// Despawns: generational ids. 2^20 slots need 20 bits, leaving 12 for the
// generation in a 32-bit id, stored in 16 bits and wrapped at 12.
// CHECK: // Entities: an id is `generation << ECS__SLOT_BITS | slot`
// CHECK: typedef uint32_t ecs_entity;
// CHECK: #define ECS__SLOT_BITS 20
// CHECK: #define ECS__GENERATION_MASK 4095
// CHECK: #define ECS__GENERATION ((uint16_t *)
ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 1048576
ecs.system @cull() reads [@P] writes [@A] {
  ecs.query (%p: !ecs.ref<@P>) {
    ecs.despawn
  }
}

// -----

// With 2^25 slots only 7 generation bits would be left, fewer than the 8
// asked for by default: the id widens to 64 bits with 32 generation bits.
// CHECK: typedef uint64_t ecs_entity;
// CHECK: #define ECS__SLOT_BITS 25
// CHECK: #define ECS__GENERATION_MASK 4294967295
// CHECK: #define ECS__GENERATION ((uint32_t *)
ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 33554432
ecs.system @cull() reads [@P] writes [@A] {
  ecs.query (%p: !ecs.ref<@P>) {
    ecs.despawn
  }
}

// -----

// A module can ask for 64-bit ids, and for more generation bits.
// CHECK: typedef uint64_t ecs_entity;
// CHECK: #define ECS__SLOT_BITS 4
module attributes {ecs.entity_id_bits = 64 : i64} {
  ecs.component @P (x: f32)
  ecs.archetype @A (@P) capacity 10
  ecs.system @cull() reads [@P] writes [@A] {
    ecs.query (%p: !ecs.ref<@P>) {
      ecs.despawn
    }
  }
}

// -----

// CHECK: typedef uint64_t ecs_entity;
// CHECK: #define ECS__SLOT_BITS 20
// CHECK: #define ECS__GENERATION_MASK 4294967295
module attributes {ecs.min_generation_bits = 16 : i64} {
  ecs.component @P (x: f32)
  ecs.archetype @A (@P) capacity 1048576
  ecs.system @cull() reads [@P] writes [@A] {
    ecs.query (%p: !ecs.ref<@P>) {
      ecs.despawn
    }
  }
}
