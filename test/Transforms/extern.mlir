// Extern systems: their declared access is their access, a run is a call
// of the C function `ent_<name>` with the world first.
// RUN: ent-opt %s --ent-print-access -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ACCESS
// RUN: ent-opt %s --ent-schedule=explain=1 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STAGES
// RUN: ent-opt %s --ent-lower-to-loops | FileCheck %s
// RUN: ent-opt %s --ent-lower-to-loops=fuse-systems=1 \
// RUN:   | FileCheck %s --check-prefix=FUSED
// RUN: ent-opt %s --ent-schedule "--ent-lower-to-loops=parallel-stages=1" \
// RUN:   | FileCheck %s --check-prefix=PARALLEL
// The whole pipeline accepts the pointer and the call.
// RUN: ent-opt %s --ent-lower-to-loops --convert-scf-to-cf --convert-to-llvm \
// RUN:     --reconcile-unrealized-casts | mlir-translate --mlir-to-llvmir \
// RUN:   | FileCheck %s --check-prefix=LLVM

ent.component @Position (x: f32)
ent.component @Sprite (id: i32)
ent.component @Stunned (seconds: f32)
ent.resource @Console (lines: i64)
ent.resource @Clock (frame: i64)
ent.relation @Follows () capacity 8
ent.archetype @Ship (@Position, @Sprite, optional @Stunned) capacity 8
ent.archetype @Rock (@Position) capacity 8

ent.system @move(%dt: f32) {
  ent.query (%p: !ent.ref<@Position, mut>) {
    %x = ent.get %p "x" : <@Position, mut> -> f32
    %n = arith.addf %x, %dt : f32
    ent.set %p "x", %n : <@Position, mut>, f32
  }
}
ent.system @tick() {
  %f = ent.read @Clock "frame" : i64
  ent.write @Clock "frame", %f : i64
}

// Every field of a listed component in every archetype holding it, with
// the rows' count and ids and the entity table; a resource's fields.
// ACCESS: remark: reads entities, Ship.count, Ship.id, Ship.Position.x, Rock.count, Rock.id, Rock.Position.x, Ship.Sprite.id, Ship.Stunned.seconds, Ship.Stunned?; writes Console.lines
ent.extern @draw(f32, i1, !ent.entity) reads [@Position, @Sprite, @Stunned] writes [@Console]
// ACCESS: remark: reads Clock.frame; writes Console.lines
ent.extern @log() reads [@Clock] writes [@Console]
// An archetype in `writes`: it spawns. A relation: it connects.
// ACCESS: remark: reads entities; writes entities, Rock.count, Rock.id, Rock.Position.x, Follows.edges
ent.extern @load() writes [@Rock, @Follows]
// ACCESS: remark: declares no access, so the system conflicts with every other system
ent.extern @beep()

// STAGES-DAG: @draw waits for @move: it reads Ship.Position.x, which @move writes
// Output is not in the contract; writing a common resource keeps the order.
// STAGES-DAG: @log waits for @draw: it writes Console.lines, which @draw also writes
// STAGES-DAG: @load waits for @draw: it writes entities, which @draw reads
// STAGES-DAG: @beep waits for @log: it has effects outside component access ('ent.extern')
// STAGES-LABEL: ent.schedule @frame
// STAGES:      ent.stage {
// STAGES-NEXT:   ent.run @move
// STAGES-NEXT:   ent.run @tick()
// STAGES-NEXT: }
// STAGES-NEXT: ent.stage {
// STAGES-NEXT:   ent.run @draw
// STAGES-NEXT: }
// STAGES-NEXT: ent.stage {
// STAGES-NEXT:   ent.run @log()
// STAGES-NEXT:   ent.run @load()
// STAGES-NEXT: }

// A bool crosses as a byte, an entity as its id.
// CHECK-DAG: func.func private @ent_draw(!llvm.ptr, f32, i8, i32)
// CHECK-DAG: func.func private @ent_log(!llvm.ptr)
// CHECK-DAG: func.func private @ent_load(!llvm.ptr)
// CHECK-DAG: func.func private @ent_beep(!llvm.ptr)
// CHECK-NOT: ent.extern
// CHECK: func.func @frame(%[[DT:.*]]: f32, %[[ON:.*]]: i1, %[[WHO:.*]]: i32, %[[ARENA:.*]]: memref<{{.*}}xi8>)
// CHECK:      call @move(%[[DT]], %[[ARENA]])
// CHECK:      scf.execute_region {
// CHECK-NEXT:   %[[ADDRESS:.*]] = memref.extract_aligned_pointer_as_index %[[ARENA]]
// CHECK-NEXT:   %[[BITS:.*]] = arith.index_cast %[[ADDRESS]] : index to i64
// CHECK-NEXT:   %[[WORLD:.*]] = llvm.inttoptr %[[BITS]] : i64 to !llvm.ptr
// CHECK-NEXT:   %[[BYTE:.*]] = arith.extui %[[ON]] : i1 to i8
// CHECK-NEXT:   call @ent_draw(%[[WORLD]], %[[DT]], %[[BYTE]], %[[WHO]])
// CHECK-NEXT:   scf.yield
// CHECK:      call @ent_log(
// The edges it may have connected are sorted before anything visits them.
// CHECK:      call @ent_load(
// CHECK-NEXT: call @ent_sort_Follows(%[[ARENA]])
// CHECK:      scf.if %[[ON]] {
// CHECK:        call @ent_log(
// CHECK:      call @ent_beep(
// CHECK-NEXT: call @ent_sort_Follows(%[[ARENA]])

// An extern system has no body to fuse; the systems around it still are.
// FUSED-LABEL: func.func @frame(
// FUSED:      scf.for
// FUSED:      call @tick(
// FUSED:      call @ent_draw(

// With stages in parallel a run stays one op of its section.
// PARALLEL-LABEL: func.func @frame(
// PARALLEL:      call @ent_draw(
// PARALLEL:      omp.section {
// PARALLEL-NEXT:   scf.execute_region {
// PARALLEL:          call @ent_log(
// PARALLEL:      omp.section {
// PARALLEL-NEXT:   scf.execute_region {
// PARALLEL:          call @ent_load(
// PARALLEL-NEXT:     call @ent_sort_Follows(

// LLVM: declare void @ent_draw(ptr, float, i8, i32)
// LLVM: call void @ent_draw(ptr %{{.*}}, float %{{.*}}, i8 %{{.*}}, i32 %{{.*}})
ent.schedule @frame(%dt: f32, %on: i1, %who: !ent.entity) {
  ent.run @move(%dt) : f32
  ent.run @tick()
  ent.run @draw(%dt, %on, %who) : f32, i1, !ent.entity
  ent.run @log()
  ent.run @load()
  ent.run @log() if {
    ent.yield %on : i1
  }
  ent.run @beep()
}
