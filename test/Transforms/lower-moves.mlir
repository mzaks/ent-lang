// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s

// Adding N moves entities of E1 to E2 and overwrites N in E2: the storage
// decides, the system is the same for both archetypes.
ecs.component @M (m: f32)
ecs.component @N (n: f32)
ecs.archetype @E1 (@M) capacity 10
ecs.archetype @E2 (@M, @N) capacity 10

// Entities move but are never despawned, so an id is a slot of the entity
// table (20 slots: 5 bits, in a 32-bit id), and the table holds only
// locations. Layout: E1 has M.m at 1152, its ids at 2304, a pending list at
// 3456, pending actions at 4608 and the values for N.n at 5760; E2 has M.m
// at 6912, N.n at 8064 and its ids at 9216; the locations are at 10368.

// CHECK-LABEL: func.func private @promote(
// CHECK:      arith.constant 3456 : index
// CHECK-NEXT: %[[PENDING:.*]] = memref.view {{.*}} to memref<10xi32>
// CHECK:      arith.constant 4608 : index
// CHECK-NEXT: %[[ACTIONS:.*]] = memref.view {{.*}} to memref<10xi32>
// CHECK:      arith.constant 5760 : index
// CHECK-NEXT: %[[VALUES:.*]] = memref.view {{.*}} to memref<10xf32>
// Views are created at the entry in the order they are first needed.
// CHECK:      arith.constant 8064 : index
// CHECK-NEXT: %[[E2_N:.*]] = memref.view {{.*}} to memref<10xf32>
// CHECK:      arith.constant 6912 : index
// CHECK-NEXT: %[[E2_M:.*]] = memref.view {{.*}} to memref<10xf32>
// CHECK:      arith.constant 9216 : index
// CHECK-NEXT: %[[E2_IDS:.*]] = memref.view {{.*}} to memref<10xi32>

// In E1, the add is a move: the row is listed with action 1 and the value.
// CHECK:      scf.for
// CHECK:        memref.store %{{.*}}, %[[PENDING]][%[[SLOT:.*]]] : memref<10xi32>
// CHECK-NEXT:   %[[MOVE:.*]] = arith.constant 1 : i32
// CHECK-NEXT:   memref.store %[[MOVE]], %[[ACTIONS]][%[[SLOT]]] : memref<10xi32>
// CHECK-NEXT:   memref.store %[[V:.*]], %[[VALUES]][%[[SLOT]]] : memref<10xf32>
// In E2, the entity already has N: overwrite.
// CHECK:      scf.for %[[J:.*]] =
// CHECK-NEXT:   memref.store %{{.*}}, %[[E2_N]][%[[J]]] : memref<10xf32>
// After both loops, E1's pending rows are applied: a move appends the
// entity to E2 (checking E2's capacity), with M copied, N from the listed
// value, its id, and its new location.
// Nothing is ever despawned, so a pending row can only be a move.
// CHECK:      scf.for
// CHECK:        scf.if
// CHECK-NOT:    scf.if
// CHECK:          cf.assert %{{.*}}, "moving an entity exceeds the capacity of @E2"
// CHECK:          memref.store %{{.*}}, %[[E2_M]]
// CHECK:          %[[N:.*]] = memref.load %[[VALUES]]
// CHECK-NEXT:     memref.store %[[N]], %[[E2_N]]
// CHECK-NEXT:     memref.store %{{.*}}, %[[E2_IDS]]
// The new location packs E2's index above the row: 1 << 4 (capacity 10
// needs 4 row bits).
// CHECK:          %[[E2_BITS:.*]] = arith.constant 16 : i32
// CHECK:          %[[PACKED:.*]] = arith.ori %[[E2_BITS]], %{{.*}} : i32
// CHECK-NEXT:     memref.store %[[PACKED]]
ecs.system @promote(%v: f32) reads [@M] writes [@N] {
  ecs.query (%m: !ecs.ref<@M>) {
    ecs.add @N(%v) : f32
  }
}
