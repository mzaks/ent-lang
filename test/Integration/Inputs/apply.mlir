// Applies from a masked, entity-local query (it runs in parallel when
// forced): sums, minima and maxima into a Total, an apply under an `if`,
// a lookup of the field being applied to, and targets that cannot take
// the values. Nothing is despawned or moved, so ids are Rows ids.

ent.component @Amount (v: f32, n: i32, seen: f32)
ent.component @Armed ()
ent.component @Target (entity: !ent.entity)
ent.component @Total (sum: f32, lo: i32, hi: i32)
ent.component @Tag ()

ent.archetype @Source (@Amount, @Target, optional @Armed) capacity 8
ent.archetype @Sink (@Total) capacity 4
ent.archetype @Gate (@Tag, optional @Total) capacity 4

ent.system @send() reads [@Target, @Armed] writes [@Amount, @Total] {
  ent.query (%a: !ent.ref<@Amount, mut>, %t: !ent.ref<@Target>,
             %armed: !ent.ref<@Armed>) {
    %target = ent.get %t "entity" : !ent.ref<@Target> -> !ent.entity
    %v = ent.get %a "v" : !ent.ref<@Amount, mut> -> f32
    %n = ent.get %a "n" : !ent.ref<@Amount, mut> -> i32
    // Every source sees the sum before any apply of this query.
    %seen, %found = ent.lookup %target @Total "sum" : f32
    ent.set %a "seen", %seen : !ent.ref<@Amount, mut>, f32
    ent.apply %target @Total "sum" add %v : f32
    ent.apply %target @Total "lo" min %n : i32
    %zero = arith.constant 0 : i32
    %positive = arith.cmpi sgt, %n, %zero : i32
    scf.if %positive {
      ent.apply %target @Total "hi" max %n : i32
    }
  }
}

ent.schedule @frame() {
  ent.run @send()
}
