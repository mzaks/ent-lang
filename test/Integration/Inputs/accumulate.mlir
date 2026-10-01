// Accumulates into a resource from a masked, entity-local query (it runs in
// parallel when forced): a float sum, an integer minimum and a count under
// an `if`, and a read of the resource, which sees its value from before
// the query.

ecs.component @Amount (v: f32, n: i32, seen: f32)
ecs.component @Armed ()
ecs.resource @Total (sum: f32, lo: i32, count: i64)

ecs.archetype @Source (@Amount, optional @Armed) capacity 8

ecs.system @send() reads [@Armed] writes [@Amount, @Total] {
  ecs.query (%a: !ecs.ref<@Amount, mut>, %armed: !ecs.ref<@Armed>) {
    %v = ecs.get %a "v" : !ecs.ref<@Amount, mut> -> f32
    %n = ecs.get %a "n" : !ecs.ref<@Amount, mut> -> i32
    %seen = ecs.read @Total "sum" : f32
    ecs.set %a "seen", %seen : !ecs.ref<@Amount, mut>, f32
    ecs.accumulate @Total "sum" add %v : f32
    ecs.accumulate @Total "lo" min %n : i32
    %zero = arith.constant 0 : i32
    %positive = arith.cmpi sgt, %n, %zero : i32
    scf.if %positive {
      %one = arith.constant 1 : i64
      ecs.accumulate @Total "count" add %one : i64
    }
  }
}

ecs.schedule @frame() {
  ecs.run @send()
}
