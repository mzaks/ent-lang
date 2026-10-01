// Accumulates into a resource from a masked, entity-local query (it runs in
// parallel when forced): a float sum, an integer minimum and a count under
// an `if`, and a read of the resource, which sees its value from before
// the query.

ent.component @Amount (v: f32, n: i32, seen: f32)
ent.component @Armed ()
ent.resource @Total (sum: f32, lo: i32, count: i64)

ent.archetype @Source (@Amount, optional @Armed) capacity 8

ent.system @send() reads [@Armed] writes [@Amount, @Total] {
  ent.query (%a: !ent.ref<@Amount, mut>, %armed: !ent.ref<@Armed>) {
    %v = ent.get %a "v" : !ent.ref<@Amount, mut> -> f32
    %n = ent.get %a "n" : !ent.ref<@Amount, mut> -> i32
    %seen = ent.read @Total "sum" : f32
    ent.set %a "seen", %seen : !ent.ref<@Amount, mut>, f32
    ent.accumulate @Total "sum" add %v : f32
    ent.accumulate @Total "lo" min %n : i32
    %zero = arith.constant 0 : i32
    %positive = arith.cmpi sgt, %n, %zero : i32
    scf.if %positive {
      %one = arith.constant 1 : i64
      ent.accumulate @Total "count" add %one : i64
    }
  }
}

ent.schedule @frame() {
  ent.run @send()
}
