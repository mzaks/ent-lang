// RUN: ent-opt %s --ent-lower-to-loops=parallel-entities=1 \
// RUN:   | FileCheck %s --check-prefix=DEFAULT
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s --check-prefix=ALWAYS
// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 fuse-systems=1" \
// RUN:   | FileCheck %s --check-prefix=FUSED

ent.component @S (x: f32)
ent.component @L (x: f32)
ent.archetype @Small (@S) capacity 1000
ent.archetype @Large (@L) capacity 2000000
ent.buffer @Cells (next: i32) capacity 4096

func.func private @log()

// The capacity is below the default threshold of 1e6: a parallel loop could
// never pay for its fork, so the loop is sequential without a check.
// DEFAULT-LABEL: func.func private @shiftSmall(
// DEFAULT-NOT:   scf.if
// DEFAULT-NOT:   scf.parallel
// DEFAULT:       scf.for
// ALWAYS-LABEL:  func.func private @shiftSmall(
// ALWAYS-NOT:    scf.if
// ALWAYS:        scf.parallel
ent.system @shiftSmall(%d: f32) writes [@S] {
  ent.query (%s: !ent.ref<@S, mut>) {
    %x = ent.get %s "x" : !ent.ref<@S, mut> -> f32
    %n = arith.addf %x, %d : f32
    ent.set %s "x", %n : !ent.ref<@S, mut>, f32
  }
}

// The capacity reaches the threshold: the count decides at run time.
// DEFAULT-LABEL: func.func private @shiftLarge(
// DEFAULT:       %[[N:.*]] = arith.index_cast
// DEFAULT:       %[[T:.*]] = arith.constant 1000000 : index
// DEFAULT:       %[[BIG:.*]] = arith.cmpi sge, %[[N]], %[[T]] : index
// DEFAULT:       scf.if %[[BIG]] {
// DEFAULT:         scf.parallel (%[[I:.*]]) = (%{{.*}}) to (%[[N]])
// DEFAULT:           memref.load %{{.*}}[%[[I]]]
// DEFAULT:           memref.store %{{.*}}, %{{.*}}[%[[I]]]
// DEFAULT:       } else {
// DEFAULT:         scf.for %[[J:.*]] = %{{.*}} to %[[N]]
// DEFAULT:           memref.load %{{.*}}[%[[J]]]
// DEFAULT:           memref.store %{{.*}}, %{{.*}}[%[[J]]]
// ALWAYS-LABEL:  func.func private @shiftLarge(
// ALWAYS-NOT:    scf.if
// ALWAYS:        scf.parallel
ent.system @shiftLarge(%d: f32) writes [@L] {
  ent.query (%l: !ent.ref<@L, mut>) {
    %x = ent.get %l "x" : !ent.ref<@L, mut> -> f32
    %n = arith.addf %x, %d : f32
    ent.set %l "x", %n : !ent.ref<@L, mut>, f32
  }
}

// A call in the body may have effects shared across entities: stay serial
// whatever the size.
// ALWAYS-LABEL: func.func private @shiftAndLog(
// ALWAYS-NOT:   scf.parallel
// ALWAYS:       scf.for
// ALWAYS:         call @log()
ent.system @shiftAndLog() writes [@L] {
  ent.query (%l: !ent.ref<@L, mut>) {
    func.call @log() : () -> ()
  }
}

// Fused loops follow the same rule per archetype.
// FUSED-LABEL: func.func @frame(
// FUSED-NOT:   scf.parallel
// FUSED:       scf.for
// FUSED:       scf.if
// FUSED:         scf.parallel
// FUSED:       } else {
// FUSED:         scf.for
// FUSED:       call @shiftAndLog(
// Memory the body allocates itself (as `nearest k` keeps its list) is the
// entity's own: written in its turn, the loop may still be parallel. Memory
// from outside the query is shared: serial.
// ALWAYS-LABEL: func.func private @ownScratch(
// ALWAYS:       scf.parallel
// ALWAYS:         memref.alloc
ent.system @ownScratch() writes [@L] {
  ent.query (%l: !ent.ref<@L, mut>) {
    %x = ent.get %l "x" : !ent.ref<@L, mut> -> f32
    %c0 = arith.constant 0 : index
    %m = memref.alloc() : memref<1xf32>
    memref.store %x, %m[%c0] : memref<1xf32>
    %y = memref.load %m[%c0] : memref<1xf32>
    memref.dealloc %m : memref<1xf32>
    ent.set %l "x", %y : !ent.ref<@L, mut>, f32
  }
}

// ALWAYS-LABEL: func.func private @sharedScratch(
// ALWAYS-NOT:   scf.parallel
// ALWAYS:       scf.for
ent.system @sharedScratch() writes [@L] {
  %m = memref.alloc() : memref<1xf32>
  ent.query (%l: !ent.ref<@L, mut>) {
    %x = ent.get %l "x" : !ent.ref<@L, mut> -> f32
    %c0 = arith.constant 0 : index
    memref.store %x, %m[%c0] : memref<1xf32>
  }
  memref.dealloc %m : memref<1xf32>
}

// A loop in the body over the rows of a buffer (a walk over the cells of
// an index): as much work for each entity as the buffer has rows, so 1000
// entities with 4096 rows reach the threshold, the two counts multiplied.
// DEFAULT-LABEL: func.func private @walkSmall(
// DEFAULT:       %[[ROWS:.*]] = arith.index_cast
// DEFAULT:       %[[N:.*]] = arith.index_cast
// DEFAULT:       %[[T:.*]] = arith.constant 1000000 : index
// DEFAULT:       %[[W:.*]] = arith.muli %[[N]], %[[ROWS]] : index
// DEFAULT:       %[[BIG:.*]] = arith.cmpi sge, %[[W]], %[[T]] : index
// DEFAULT:       scf.if %[[BIG]] {
// DEFAULT:         scf.parallel
// DEFAULT:           scf.while
// DEFAULT:       } else {
// DEFAULT:         scf.for
ent.system @walkSmall() reads [@Cells] writes [@S] {
  ent.query (%s: !ent.ref<@S, mut>) {
    %c0 = arith.constant 0 : i32
    %end = scf.while (%at = %c0) : (i32) -> i32 {
      %n = ent.buffer.len @Cells
      %more = arith.cmpi slt, %at, %n : i32
      scf.condition(%more) %at : i32
    } do {
    ^bb0(%at: i32):
      %next = ent.buffer.at @Cells "next"[%at : i32] : i32
      scf.yield %next : i32
    }
    %f = arith.sitofp %end : i32 to f32
    ent.set %s "x", %f : !ent.ref<@S, mut>, f32
  }
}

ent.schedule @frame(%d: f32) {
  ent.run @shiftSmall(%d) : f32
  ent.run @shiftLarge(%d) : f32
  ent.run @shiftAndLog()
  ent.run @ownScratch()
  ent.run @sharedScratch()
  ent.run @walkSmall()
}
