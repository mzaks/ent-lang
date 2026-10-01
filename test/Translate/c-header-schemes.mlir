// RUN: ent-translate --ent-to-c-header %s -split-input-file | FileCheck %s

// The entity scheme follows from the structural changes and capacities.

// Moves but no despawns: an id is a slot (2000 slots need 11 bits), the
// table holds only locations, and slots are never reused.
// CHECK: // Entities: entities move but are never despawned
// CHECK: typedef uint32_t ent_entity;
// CHECK: #define ENT__SLOT_BITS 11
// CHECK-NOT: ENT__GENERATION
// CHECK: #define ENT__LOCATION ((uint32_t *)
// CHECK: static inline ent_entity ent__allocate(
// CHECK:        int64_t slot = (*ENT__NEXT_SLOT)++;
// CHECK: return id != ENT_NO_ENTITY && (int64_t)ent__slot(id) < *ENT__NEXT_SLOT;
// CHECK: static inline ent_entity *ent_Calm_id(
ent.component @P (x: f32)
ent.component @S (s: f32)
ent.archetype @Calm (@P) capacity 1000
ent.archetype @Stunned (@P, @S) capacity 1000
ent.system @stun(%s: f32) reads [@P] writes [@S] {
  ent.query (%p: !ent.ref<@P>) {
    ent.add @S(%s) : f32
  }
}

// -----

// Despawns: generational ids. 2^20 slots need 20 bits, leaving 12 for the
// generation in a 32-bit id, stored in 16 bits and wrapped at 12.
// CHECK: // Entities: an id is `generation << ENT__SLOT_BITS | slot`
// CHECK: typedef uint32_t ent_entity;
// CHECK: #define ENT__SLOT_BITS 20
// CHECK: #define ENT__GENERATION_MASK 4095
// CHECK: #define ENT__GENERATION ((uint16_t *)
ent.component @P (x: f32)
ent.archetype @A (@P) capacity 1048576
ent.system @cull() reads [@P] writes [@A] {
  ent.query (%p: !ent.ref<@P>) {
    ent.despawn
  }
}

// -----

// With 2^25 slots only 7 generation bits would be left, fewer than the 8
// asked for by default: the id widens to 64 bits with 32 generation bits.
// CHECK: typedef uint64_t ent_entity;
// CHECK: #define ENT__SLOT_BITS 25
// CHECK: #define ENT__GENERATION_MASK 4294967295
// CHECK: #define ENT__GENERATION ((uint32_t *)
ent.component @P (x: f32)
ent.archetype @A (@P) capacity 33554432
ent.system @cull() reads [@P] writes [@A] {
  ent.query (%p: !ent.ref<@P>) {
    ent.despawn
  }
}

// -----

// A module can ask for 64-bit ids, and for more generation bits.
// CHECK: typedef uint64_t ent_entity;
// CHECK: #define ENT__SLOT_BITS 4
module attributes {ent.entity_id_bits = 64 : i64} {
  ent.component @P (x: f32)
  ent.archetype @A (@P) capacity 10
  ent.system @cull() reads [@P] writes [@A] {
    ent.query (%p: !ent.ref<@P>) {
      ent.despawn
    }
  }
}

// -----

// CHECK: typedef uint64_t ent_entity;
// CHECK: #define ENT__SLOT_BITS 20
// CHECK: #define ENT__GENERATION_MASK 4294967295
module attributes {ent.min_generation_bits = 16 : i64} {
  ent.component @P (x: f32)
  ent.archetype @A (@P) capacity 1048576
  ent.system @cull() reads [@P] writes [@A] {
    ent.query (%p: !ent.ref<@P>) {
      ent.despawn
    }
  }
}
