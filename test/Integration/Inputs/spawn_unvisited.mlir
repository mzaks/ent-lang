// A query visits the entities that exist when it starts (docs/sync-points.md,
// Q1). It matches @A and @B, and every entity it visits for the first time
// spawns a @B: the one spawned while visiting @A must not be visited by the
// loop over @B that follows in the same query.
ent.component @T (visits: i32)
ent.component @X ()
ent.archetype @A (@T, @X) capacity 8
ent.archetype @B (@T) capacity 8
ent.system @s() writes [@T, @B] {
  ent.query (%t: !ent.ref<@T, mut>) {
    %v = ent.get %t "visits" : !ent.ref<@T, mut> -> i32
    %one = arith.constant 1 : i32
    %n = arith.addi %v, %one : i32
    ent.set %t "visits", %n : !ent.ref<@T, mut>, i32
    %zero = arith.constant 0 : i32
    %first = arith.cmpi eq, %v, %zero : i32
    scf.if %first {
      %id = ent.spawn @B(%zero) : i32
    }
  }
}
ent.schedule @frame() {
  ent.run @s()
}
