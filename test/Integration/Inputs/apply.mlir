// Applies from a masked, entity-local query (it runs in parallel when
// forced): sums, minima and maxima into a Total, an apply under an `if`,
// a lookup of the field being applied to, and targets that cannot take
// the values. Nothing is despawned or moved, so ids are Rows ids.

ecs.component @Amount (v: f32, n: i32, seen: f32)
ecs.component @Armed ()
ecs.component @Target (entity: !ecs.entity)
ecs.component @Total (sum: f32, lo: i32, hi: i32)
ecs.component @Tag ()

ecs.archetype @Source (@Amount, @Target, optional @Armed) capacity 8
ecs.archetype @Sink (@Total) capacity 4
ecs.archetype @Gate (@Tag, optional @Total) capacity 4

ecs.system @send() reads [@Target, @Armed] writes [@Amount, @Total] {
  ecs.query (%a: !ecs.ref<@Amount, mut>, %t: !ecs.ref<@Target>,
             %armed: !ecs.ref<@Armed>) {
    %target = ecs.get %t "entity" : !ecs.ref<@Target> -> !ecs.entity
    %v = ecs.get %a "v" : !ecs.ref<@Amount, mut> -> f32
    %n = ecs.get %a "n" : !ecs.ref<@Amount, mut> -> i32
    // Every source sees the sum before any apply of this query.
    %seen, %found = ecs.lookup %target @Total "sum" : f32
    ecs.set %a "seen", %seen : !ecs.ref<@Amount, mut>, f32
    ecs.apply %target @Total "sum" add %v : f32
    ecs.apply %target @Total "lo" min %n : i32
    %zero = arith.constant 0 : i32
    %positive = arith.cmpi sgt, %n, %zero : i32
    scf.if %positive {
      ecs.apply %target @Total "hi" max %n : i32
    }
  }
}

ecs.schedule @frame() {
  ecs.run @send()
}
