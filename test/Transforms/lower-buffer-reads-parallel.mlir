// RUN: ent-opt %s "--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1" \
// RUN:   | FileCheck %s

// A buffer is read like a resource: no query changes one while it runs.
// So a query that reads a buffer's rows, itself or through a fn that is
// given the buffer, visits its entities in parallel like one that reads
// a resource. (One that calls a proc still visits them one at a time.)

ent.component @P (x: f32, at: i32)
ent.archetype @Ps (@P) capacity 100
ent.buffer @Curve (part: f32) capacity 200

ent.function @part(%curve: !ent.buffer<@Curve>, %at: i32) -> f32 {
  %0 = ent.buffer.at @Curve "part"[%at : i32] : f32
  ent.yield %0 : f32
}
ent.function proc @show(f32)

// CHECK-LABEL: func.func private @through_fn(
// CHECK:       scf.parallel
// CHECK:         call @ent_part(
ent.system @through_fn() {
  ent.query (%p: !ent.ref<@P, mut>) {
    %at = ent.get %p "at" : <@P, mut> -> i32
    %curve = ent.buffer.of @Curve : <@Curve>
    %part = ent.invoke @part(%curve, %at) : (!ent.buffer<@Curve>, i32) -> f32
    ent.set %p "x", %part : <@P, mut>, f32
  }
}

// CHECK-LABEL: func.func private @itself(
// CHECK:       scf.parallel
ent.system @itself() {
  ent.query (%p: !ent.ref<@P, mut>) {
    %at = ent.get %p "at" : <@P, mut> -> i32
    %part = ent.buffer.at @Curve "part"[%at : i32] : f32
    ent.set %p "x", %part : <@P, mut>, f32
  }
}

// CHECK-LABEL: func.func private @to_proc(
// CHECK-NOT:   scf.parallel
// CHECK:       return
ent.system @to_proc() {
  ent.query (%p: !ent.ref<@P, mut>) {
    %at = ent.get %p "at" : <@P, mut> -> i32
    %part = ent.buffer.at @Curve "part"[%at : i32] : f32
    ent.invoke proc @show(%part) : (f32) -> ()
  }
}
