// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 --symbol-dce | FileCheck %s

ent.component @H (hp: f32, lvl: i32)
ent.component @T (target: !ent.entity)
ent.archetype @Ship (@H) capacity 16
ent.archetype @Gun (@T) capacity 8
ent.resource @Score (points: i64)

// Guns send to their ships' hit points.
ent.system @shoot(%d: f32) reads [@T] writes [@H] {
  ent.query (%t: !ent.ref<@T>) {
    %id = ent.get %t "target" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
  }
}
ent.system @burn(%d: f32) reads [@T] writes [@H] {
  ent.query (%t: !ent.ref<@T>) {
    %id = ent.get %t "target" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" min %d : f32
  }
}
// Reads and writes another field of the ships.
ent.system @level() writes [@H] {
  ent.query (%h: !ent.ref<@H, mut>) {
    %l = ent.get %h "lvl" : !ent.ref<@H, mut> -> i32
    %one = arith.constant 1 : i32
    %n = arith.addi %l, %one : i32
    ent.set %h "lvl", %n : !ent.ref<@H, mut>, i32
  }
}
// Reads and writes the hit points the guns send to.
ent.system @heal() writes [@H] {
  ent.query (%h: !ent.ref<@H, mut>) {
    %p = ent.get %h "hp" : !ent.ref<@H, mut> -> f32
    %n = arith.addf %p, %p : f32
    ent.set %h "hp", %n : !ent.ref<@H, mut>, f32
  }
}
// Counts the guns into a resource.
ent.system @count() reads [@T] writes [@Score] {
  ent.query (%t: !ent.ref<@T>) {
    %one = arith.constant 1 : i64
    ent.accumulate @Score "points" add %one : i64
  }
}
// Reads that resource outside its query.
ent.system @report() reads [@Score] writes [@H] {
  %s = ent.read @Score "points" : i64
  %l = arith.trunci %s : i64 to i32
  ent.query (%h: !ent.ref<@H, mut>) {
    ent.set %h "lvl", %l : !ent.ref<@H, mut>, i32
  }
}

// CHECK-NOT: func.func private @shoot
// CHECK-NOT: func.func private @burn
// CHECK-NOT: func.func private @level
// CHECK-NOT: func.func private @heal
// CHECK-NOT: func.func private @count
// CHECK-NOT: func.func private @report

// A system that sends fuses with one that does not touch what it sends:
// a loop for each archetype (the ships', then the guns'), the hit points
// combined in the guns' loop as it goes (nothing else in the fused loops
// touches them: no buffer, no loop after).
// CHECK-LABEL: func.func @apart(
// CHECK:       scf.for
// CHECK:         arith.addi
// CHECK:       scf.for
// CHECK:         arith.addf
// CHECK-NOT:   scf.for
// CHECK:       return
ent.schedule @apart(%d: f32) {
  ent.run @shoot(%d) : f32
  ent.run @level()
}

// Two that send to the same field are not fused: fused, the sends of both
// would go to their buffers and land after the loop, where each alone
// combines them as its loop goes. So a loop for each, each combining.
// CHECK-LABEL: func.func @both(
// CHECK:       scf.for
// CHECK:         arith.addf
// CHECK:       scf.for
// CHECK:         arith.minimumf
// CHECK-NOT:   scf.for
// CHECK:       return
ent.schedule @both(%d: f32) {
  ent.run @shoot(%d) : f32
  ent.run @burn(%d) : f32
}

// One that reads what the sequence sends ends it: the hit points land
// before the ships' loop that doubles them.
// CHECK-LABEL: func.func @after(
// CHECK:       scf.for
// CHECK:         arith.addf %{{.*}}, %arg0
// CHECK:       scf.for
// CHECK:         %[[HP:.*]] = memref.load
// CHECK-NEXT:    arith.addf %[[HP]], %[[HP]]
// CHECK:       return
ent.schedule @after(%d: f32) {
  ent.run @shoot(%d) : f32
  ent.run @heal()
}

// Code outside the queries runs ahead of the fused loop: a system that
// reads there what the sequence adds into ends it too. The count is
// carried through the guns' loop and stored before it is read.
// CHECK-LABEL: func.func @counted(
// CHECK:       scf.for {{.*}} iter_args
// CHECK:       memref.store
// CHECK:       memref.load
// CHECK:       arith.trunci
// CHECK:       scf.for
// CHECK:       return
ent.schedule @counted() {
  ent.run @count()
  ent.run @report()
}
